"""Bounded GHCR range transfer; credentials/redirects never leave job memory."""
import argparse
import base64
import concurrent.futures
import contextlib
import fcntl
import hashlib
import http.client
import io
import json
import os
from pathlib import Path
import re
import shutil
import ssl
import stat
import tarfile
import threading
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

DIGEST = re.compile(r'sha256:[0-9a-f]{64}')
IMAGE = re.compile(r'ghcr.io/(duan-jia/shuming-(?:backend|mcp)):([0-9a-f]{40})')
REDIRECT_HOSTS = {'ghcr.io', 'pkg-containers.githubusercontent.com'}
MANIFESTS = {'application/vnd.oci.image.manifest.v1+json', 'application/vnd.docker.distribution.manifest.v2+json'}
INDEXES = {'application/vnd.oci.image.index.v1+json', 'application/vnd.docker.distribution.manifest.list.v2+json'}
LAYERS = {'application/vnd.oci.image.layer.v1.tar+gzip', 'application/vnd.oci.image.layer.v1.tar',
          'application/vnd.docker.image.rootfs.diff.tar.gzip'}
METADATA_LIMIT = 4 * 1024 * 1024
CHUNK = 128 * 1024
WORKERS = 8
CACHE_LIMIT = 512 * 1024 * 1024


class Failure(Exception):
    """Only a fixed category is ever exposed by the CLI."""


class Retry(Failure):
    pass


class Budget:
    def __init__(self, seconds=540, clock=time.monotonic):
        self.clock = clock
        self.end = clock() + seconds
        self.cancel = threading.Event()

    def check(self):
        remaining = self.end - self.clock()
        if self.cancel.is_set() or remaining <= 0:
            raise Failure('deadline')
        return min(10, remaining)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        return None


def validate_url(url):
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != 'https' or parsed.hostname not in REDIRECT_HOSTS
            or parsed.port not in (None, 443) or parsed.username or parsed.password or parsed.fragment):
        raise Failure('redirect_policy')
    return parsed


def private(path, mode):
    info = path.lstat()
    if path.is_symlink() or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != mode:
        raise Failure('credentials_policy')
    return info


def read_auth(directory):
    directory = Path(directory)
    if not directory.is_absolute() or directory.resolve() != directory:
        raise Failure('credentials_policy')
    if not stat.S_ISDIR(private(directory, 0o700).st_mode):
        raise Failure('credentials_policy')
    config = directory / 'config.json'
    if not stat.S_ISREG(private(config, 0o600).st_mode) or config.stat().st_size > 65536:
        raise Failure('credentials_policy')
    value = json.loads(config.read_text())['auths']['ghcr.io']['auth']
    decoded = base64.b64decode(value, validate=True)
    if b':' not in decoded or b'\n' in decoded or b'\r' in decoded:
        raise Failure('credentials_policy')
    return value


def read_some(response, size):
    # read1 avoids waiting for a whole buffer from a slow trickle. Recheck the
    # image-wide budget between socket reads; outer570s is the hard backstop.
    return getattr(response, 'read1', response.read)(size)


class Registry:
    def __init__(self, repository, basic, budget, opener=None):
        if repository not in ('duan-jia/shuming-backend', 'duan-jia/shuming-mcp'):
            raise Failure('image_policy')
        self.repository, self.basic, self.budget = repository, basic, budget
        self.opener = opener or urllib.request.build_opener(NoRedirect())
        self.token, self.expiry = '', 0
        self.lock = threading.Lock()

    def open(self, url, headers):
        validate_url(url)
        request = urllib.request.Request(url, headers=headers)
        try:
            return self.opener.open(request, timeout=self.budget.check())
        except urllib.error.HTTPError as error:
            return error
        except ssl.SSLError:
            raise Retry('tls') from None
        except (OSError, urllib.error.URLError, http.client.HTTPException):
            raise Retry('io') from None

    def small(self, response):
        blocks, size = [], 0
        with response:
            while size <= METADATA_LIMIT:
                self.budget.check()
                block = read_some(response, min(65536, METADATA_LIMIT + 1 - size))
                if not block:
                    break
                blocks.append(block)
                size += len(block)
        raw = b''.join(blocks)
        if len(raw) > METADATA_LIMIT:
            raise Failure('metadata')
        return raw

    def authorize(self, challenge, rejected):
        if not challenge.lower().startswith('bearer '):
            raise Failure('auth_policy')
        fields = urllib.request.parse_http_list(challenge[7:])
        params = urllib.request.parse_keqv_list(fields)
        if len(fields) != len(params) or params.get('realm') != 'https://ghcr.io/token' or params.get('service') != 'ghcr.io':
            raise Failure('auth_policy')
        scope = 'repository:' + self.repository + ':pull'
        if params.get('scope', scope) != scope:
            raise Failure('auth_policy')
        with self.lock:
            if self.token != rejected and self.expiry > self.budget.clock():
                return
            url = 'https://ghcr.io/token?' + urllib.parse.urlencode({'service': 'ghcr.io', 'scope': scope})
            response = self.open(url, {'Authorization': 'Basic ' + self.basic})
            if response.status != 200:
                response.close()
                raise Failure('auth')
            data = json.loads(self.small(response))
            token = data.get('token') or data.get('access_token')
            if not isinstance(token, str) or not token or len(token) > 65536 or '\r' in token or '\n' in token:
                raise Failure('auth')
            seconds = data.get('expires_in', 60)
            if type(seconds) is not int or seconds <= 0:
                raise Failure('auth')
            self.token = token
            self.expiry = self.budget.clock() + max(1, min(seconds, 3600) - 5)

    def request(self, url, headers):
        if urllib.parse.urlsplit(url).netloc != 'ghcr.io':
            raise Failure('auth_policy')
        for _ in range(2):
            token = self.token if self.expiry > self.budget.clock() else ''
            outgoing = dict(headers)
            if token:
                outgoing['Authorization'] = 'Bearer ' + token
            response = self.open(url, outgoing)
            if response.status != 401:
                return response
            challenge = response.headers.get('WWW-Authenticate', '')
            response.close()
            self.authorize(challenge, token)
        raise Failure('auth')

    def manifest(self, reference, expected=None):
        url = f'https://ghcr.io/v2/{self.repository}/manifests/{reference}'
        response = self.request(url, {'Accept': ', '.join(sorted(MANIFESTS | INDEXES))})
        if response.status != 200:
            response.close()
            raise Failure('manifest_http')
        claimed = response.headers.get('Docker-Content-Digest')
        raw = self.small(response)
        actual = 'sha256:' + hashlib.sha256(raw).hexdigest()
        if (expected and actual != expected) or (claimed and actual != claimed):
            raise Failure('manifest_digest')
        return raw, json.loads(raw)

    def range(self, digest, start, end, size):
        origin = f'https://ghcr.io/v2/{self.repository}/blobs/{digest}'
        url = origin
        headers = {'Range': f'bytes={start}-{end}', 'Accept-Encoding': 'identity'}
        for redirect in range(5):
            response = self.request(url, headers) if redirect == 0 else self.open(url, headers)
            if response.status in (301, 302, 303, 307, 308):
                location = response.headers.get('Location', '')
                response.close()
                url = urllib.parse.urljoin(url, location)
                validate_url(url)
                continue  # No Authorization on ANY redirected request.
            if response.status in (401, 403) and redirect:
                response.close()
                raise Retry('address_expired')
            if response.status in (408, 429, 500, 502, 503, 504):
                response.close()
                raise Retry('http_transient')
            if response.status != 206:
                print(f'Range HTTP response: status={response.status}', flush=True)
                response.close()
                raise Failure('range_status')
            if (response.headers.get('Content-Range') != f'bytes {start}-{end}/{size}'
                    or response.headers.get('Content-Encoding', 'identity') != 'identity'
                    or response.headers.get('Content-Length', str(end - start + 1)) != str(end - start + 1)):
                response.close()
                raise Failure('range_protocol')
            return response
        raise Failure('redirect_policy')


def descriptor(value, max_size=2 * 1024 ** 3):
    if (not isinstance(value, dict) or not DIGEST.fullmatch(value.get('digest', ''))
            or type(value.get('size')) is not int or not 0 <= value['size'] <= max_size
            or value.get('urls')):
        raise Failure('metadata')
    return value


class BlobCache:
    """Private, bounded compressed blobs only; never auth, manifests or URLs.

    The caller must obtain the current manifest using the current job token.
    A hit is copied and rehashed against that manifest before it can be used.
    One nonblocking lock covers lookup, publication and eviction.
    """
    def __init__(self, directory, budget, limit=CACHE_LIMIT):
        self.directory, self.budget, self.limit = Path(directory), budget, limit
        self.lock = None

    def regular(self, path):
        info = private(path, 0o600)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise Failure('cache_policy')
        return info

    def __enter__(self):
        directory = self.directory
        if not directory.is_absolute() or directory.resolve() != directory:
            raise Failure('cache_policy')
        directory.mkdir(mode=0o700, exist_ok=True)
        if not stat.S_ISDIR(private(directory, 0o700).st_mode):
            raise Failure('cache_policy')
        self.lock = os.open(directory / '.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            if self.regular(directory / '.lock').st_size != 0:
                raise Failure('cache_policy')
            try:
                fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise Failure('cache_busy') from None
            # A killed writer may leave an unpublished temp file. Under the
            # exclusive lock only our own narrowly named regular files qualify.
            for path in directory.iterdir():
                self.budget.check()
                if re.fullmatch(r'\.partial-[a-z0-9_]+', path.name):
                    self.regular(path)
                    path.unlink()
            self.prune(0)
            return self
        except BaseException:
            os.close(self.lock)
            self.lock = None
            raise

    def __exit__(self, *args):
        if self.lock is not None:
            os.close(self.lock)
            self.lock = None

    def prune(self, reserve):
        entries = []
        for path in self.directory.iterdir():
            self.budget.check()
            if path.name == '.lock':
                continue
            if not re.fullmatch(r'[0-9a-f]{64}', path.name):
                raise Failure('cache_policy')
            info = self.regular(path)
            entries.append((info.st_mtime_ns, path.name, path, info.st_size))
        total = sum(item[3] for item in entries)
        for _, _, path, size in sorted(entries):
            if total + reserve <= self.limit:
                break
            self.budget.check()
            path.unlink()
            total -= size

    def copy(self, source, target):
        checksum = hashlib.sha256()
        with source.open('rb') as incoming, target.open('xb') as outgoing:
            os.chmod(target, 0o600)
            for block in iter(lambda: incoming.read(1024 * 1024), b''):
                self.budget.check()
                outgoing.write(block)
                checksum.update(block)
        return 'sha256:' + checksum.hexdigest()

    def take(self, item, directory):
        path = self.directory / item['digest'][7:]
        try:
            info = self.regular(path)
        except FileNotFoundError:
            return None
        target = directory / path.name
        if info.st_size == item['size']:
            digest = self.copy(path, target)
            if digest == item['digest']:
                os.utime(path, None)
                print(f'Range cache hit: verified_bytes={info.st_size}', flush=True)
                return target
            target.unlink()
        # Damaged content is a miss; unsafe ownership/types fail above.
        path.unlink()
        print('Range cache discarded: reason=size_or_digest', flush=True)
        return None

    def put(self, item, source):
        if item['size'] > self.limit:
            return  # Large blobs still transfer, without defeating the cap.
        self.prune(item['size'])
        if item['size'] > shutil.disk_usage(self.directory).free * 4 // 5:
            # The persistent directory may live on a different filesystem than
            # the job workspace. A full cache disk need not break the transfer.
            print('Range cache skipped: reason=capacity', flush=True)
            return
        fd, name = tempfile.mkstemp(prefix='.partial-', dir=self.directory)
        path = Path(name)
        os.close(fd)
        path.unlink()  # copy uses exclusive creation.
        try:
            if self.copy(source, path) != item['digest'] or path.stat().st_size != item['size']:
                raise Failure('blob_digest')
            with path.open('rb') as stream:
                os.fsync(stream.fileno())
            os.replace(path, self.directory / item['digest'][7:])
        finally:
            path.unlink(missing_ok=True)


def download_blob(client, item, directory):
    descriptor(item)
    size, digest = item['size'], item['digest']
    target = directory / digest[7:]
    partial = target.with_suffix('.partial')
    fd = os.open(partial, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    os.ftruncate(fd, size)
    counter, complete = 0, 0
    lock = threading.Lock()

    def part(start):
        nonlocal counter, complete
        end = min(start + CHUNK, size) - 1
        position = start
        for attempt in range(2):
            try:
                client.budget.check()
                with client.range(digest, position, end, size) as response:
                    while position <= end:
                        client.budget.check()
                        try:
                            data = read_some(response, min(65536, end - position + 1))
                        except http.client.IncompleteRead as error:
                            data = error.partial
                        if not data:
                            raise Retry('io')
                        if len(data) > end - position + 1:
                            raise Failure('range_protocol')
                        offset = 0
                        while offset < len(data):
                            try:
                                count = os.pwrite(fd, data[offset:], position + offset)
                            except OSError:
                                raise Failure('disk') from None
                            if count <= 0:
                                raise Failure('disk')
                            offset += count
                        position += len(data)
                        with lock:
                            counter += len(data)
                    if read_some(response, 1):
                        raise Failure('range_protocol')
                with lock:
                    complete += 1
                return
            except (Retry, OSError, http.client.HTTPException):
                if position > end:
                    raise Failure('range_protocol') from None
                if attempt == 1:
                    raise Failure('range_interrupted') from None
        raise Failure('range_interrupted')

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as pool:
            futures = [pool.submit(part, start) for start in range(0, size, CHUNK)]
            try:
                for future in concurrent.futures.as_completed(futures):
                    future.result()
            except BaseException:
                client.budget.cancel.set()
                for future in futures:
                    future.cancel()
                raise
        checksum = hashlib.sha256()
        with partial.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                client.budget.check()
                checksum.update(block)
        if 'sha256:' + checksum.hexdigest() != digest:
            raise Failure('blob_digest')
        os.fsync(fd)
        os.replace(partial, target)
        return target
    finally:
        os.close(fd)
        partial.unlink(missing_ok=True)
        print(f'Range progress: retained_bytes={counter} completed_chunks={complete} total_bytes={size}', flush=True)


def transfer(image, work, basic, budget=None, opener=None, cache_directory=None):
    match = IMAGE.fullmatch(image)
    if not match:
        raise Failure('image_policy')
    budget = budget or Budget()
    client = Registry(match[1], basic, budget, opener)
    raw, manifest = client.manifest(match[2])
    if manifest.get('mediaType') in INDEXES:
        matches = [descriptor(x, METADATA_LIMIT) for x in manifest['manifests']
                   if x.get('platform', {}).get('os') == 'linux' and x.get('platform', {}).get('architecture') == 'amd64'
                   and not x.get('platform', {}).get('variant')]
        if len(matches) != 1:
            raise Failure('platform')
        raw, manifest = client.manifest(matches[0]['digest'], matches[0]['digest'])
        if len(raw) != matches[0]['size']:
            raise Failure('manifest_digest')
    if manifest.get('schemaVersion') != 2 or manifest.get('mediaType') not in MANIFESTS:
        raise Failure('metadata')
    config = descriptor(manifest['config'], METADATA_LIMIT)
    layers = manifest['layers']
    if not isinstance(layers, list) or len(layers) > 128:
        raise Failure('metadata')
    for layer in layers:
        descriptor(layer)
        if layer.get('mediaType') not in LAYERS:
            raise Failure('metadata')
    items = {x['digest']: x for x in [config, *layers]}
    if sum(x['size'] for x in items.values()) > 4 * 1024 ** 3:
        raise Failure('capacity')
    compressed = sum(x['size'] for x in items.values())
    # Budget for job blobs, archive and newly retained cache bytes. Existing
    # cache occupancy is already excluded from disk_usage().free.
    required = 2 * compressed + (min(compressed, CACHE_LIMIT) if cache_directory else 0) + 1024 * 1024
    available = shutil.disk_usage(work).free
    if required > available * 4 // 5:
        raise Failure('capacity')
    print(f'Range image pinned: platform=linux/amd64 layers={len(layers)} compressed_bytes={sum(x["size"] for x in layers)} required_bytes={required} free_bytes={available}', flush=True)
    directory = work / 'blobs'
    directory.mkdir(mode=0o700)
    try:
        with contextlib.ExitStack() as stack:
            cache = stack.enter_context(BlobCache(cache_directory, budget)) if cache_directory else None
            paths = {}
            for digest, item in items.items():
                path = cache.take(item, directory) if cache else None
                if path is None:
                    path = download_blob(client, item, directory)
                    if cache:
                        cache.put(item, path)
                paths[digest] = path
        data = json.loads(paths[config['digest']].read_bytes())
        if data.get('os') != 'linux' or data.get('architecture') != 'amd64':
            raise Failure('platform')
        if data.get('rootfs', {}).get('type') != 'layers' or len(data['rootfs']['diff_ids']) != len(layers):
            raise Failure('metadata')
        (work / 'manifest.json').write_bytes(raw)
        partial = work / 'image.partial'
        with tarfile.open(partial, 'w') as archive:
            config_name = config['digest'][7:] + '.json'
            archive.add(paths[config['digest']], arcname=config_name)
            names = []
            for layer in layers:
                name = layer['digest'][7:] + '.tar.gz'
                archive.add(paths[layer['digest']], arcname=name)
                names.append(name)
            payload = json.dumps([{'Config': config_name, 'RepoTags': [image], 'Layers': names}]).encode()
            info = tarfile.TarInfo('manifest.json')
            info.size = len(payload)
            archive.addfile(info, io.BytesIO(payload))
        budget.check()
        os.replace(partial, work / 'image.tar')
    finally:
        client.basic = client.token = ''
        shutil.rmtree(directory)
        (work / 'image.partial').unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image')
    parser.add_argument('work', type=Path)
    args = parser.parse_args()
    os.umask(0o077)
    try:
        private(args.work, 0o700)
        if not args.work.is_absolute() or args.work.resolve() != args.work:
            raise Failure('credentials_policy')
        transfer(args.image, args.work, read_auth(os.environ['DOCKER_CONFIG']),
                 cache_directory=os.environ.get('STAGING_BLOB_CACHE') or None)
    except Failure as error:
        # Failure messages are static labels only; never print arbitrary text.
        allowed = {'deadline', 'redirect_policy', 'credentials_policy', 'image_policy', 'auth_policy',
                   'auth', 'io', 'tls', 'metadata', 'manifest_http', 'manifest_digest', 'address_expired',
                   'http_transient', 'range_status', 'range_protocol', 'range_interrupted', 'disk',
                   'blob_digest', 'platform', 'capacity', 'cache_policy', 'cache_busy'}
        label = str(error) if str(error) in allowed else 'internal'
        print('Range transfer failed category=' + label, file=__import__('sys').stderr)
        return 1
    except Exception:
        print('Range transfer failed category=internal', file=__import__('sys').stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
