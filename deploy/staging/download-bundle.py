#!/usr/bin/env python3
"""Download this workflow's artifact using bounded, parallel HTTP ranges."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import threading
import time
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        return None


def artifact(token, repository, run_id):
    opener = urllib.request.build_opener(NoRedirect)
    base = f'https://api.github.com/repos/{repository}/actions'
    headers = {'Authorization': f'Bearer {token}', 'Accept': 'application/vnd.github+json',
               'User-Agent': 'shuming-staging'}
    request = urllib.request.Request(f'{base}/runs/{run_id}/artifacts?name=staging-bundle', headers=headers)
    with opener.open(request, timeout=30) as response:
        entries = json.load(response)['artifacts']
    selected = [entry for entry in entries if entry['name'] == 'staging-bundle' and not entry['expired']]
    if len(selected) != 1:
        raise RuntimeError('Expected exactly one staging bundle in this workflow run')
    entry = selected[0]
    request = urllib.request.Request(f'{base}/artifacts/{entry["id"]}/zip', headers=headers)
    try:
        opener.open(request, timeout=30).close()
    except urllib.error.HTTPError as error:
        if error.code not in (301, 302, 303, 307, 308):
            raise
        url = error.headers['Location']
        error.close()
    else:
        raise RuntimeError('Artifact API did not return a download redirect')
    if urllib.parse.urlsplit(url).scheme != 'https':
        raise RuntimeError('Artifact download must use HTTPS')
    # The GitHub bearer token is never forwarded to the artifact storage host.
    return url, entry['size_in_bytes'], entry.get('digest', '')


def download(url, size, digest, target, *, workers=32, chunk_size=4 * 1024 * 1024, timeout=480):
    if size <= 0 or size > 2 * 1024 * 1024 * 1024 or not digest or not digest.startswith('sha256:'):
        raise ValueError('Invalid artifact size or digest')
    target = Path(target)
    partial = target.with_suffix('.partial')
    deadline = time.monotonic() + timeout
    cancelled = threading.Event()
    descriptor = os.open(partial, os.O_CREAT | os.O_TRUNC | os.O_RDWR, 0o600)
    os.ftruncate(descriptor, size)

    def chunk(start):
        end = min(size, start + chunk_size) - 1
        for attempt in range(3):
            try:
                if cancelled.is_set() or time.monotonic() >= deadline:
                    raise TimeoutError('Artifact download deadline exceeded')
                request = urllib.request.Request(url, headers={
                    'Range': f'bytes={start}-{end}', 'Accept-Encoding': 'identity'})
                with urllib.request.urlopen(request, timeout=min(30, max(1, deadline - time.monotonic()))) as response:
                    if response.status != 206 or response.headers.get('Content-Range') != f'bytes {start}-{end}/{size}':
                        raise ValueError('Artifact server did not honor the requested range')
                    position = start
                    while position <= end:
                        if cancelled.is_set() or time.monotonic() >= deadline:
                            raise TimeoutError('Artifact download deadline exceeded')
                        data = response.read(min(65536, end - position + 1))
                        if not data:
                            raise OSError('Incomplete artifact range')
                        offset = 0
                        while offset < len(data):
                            written = os.pwrite(descriptor, data[offset:], position + offset)
                            if written <= 0:
                                raise OSError('Could not write artifact range')
                            offset += written
                        position += len(data)
                return
            except urllib.error.HTTPError as error:
                error.close()
                if error.code not in (408, 429, 500, 502, 503, 504):
                    raise RuntimeError(f'Artifact storage returned HTTP {error.code}') from None
            except (OSError, TimeoutError):
                pass
            if attempt == 2 or time.monotonic() >= deadline or cancelled.is_set():
                raise RuntimeError(f'Artifact range at offset {start} failed after bounded retries')
            cancelled.wait(attempt + 1)

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures = [pool.submit(chunk, start) for start in range(0, size, chunk_size)]
            try:
                for future in concurrent.futures.as_completed(futures):
                    future.result()
            except Exception:
                cancelled.set()
                for future in futures:
                    future.cancel()
                raise
        with partial.open('rb') as source:
            checksum = hashlib.file_digest(source, 'sha256').hexdigest()
        if checksum != digest.removeprefix('sha256:'):
            raise RuntimeError('Artifact SHA-256 digest does not match')
        os.replace(partial, target)
    finally:
        os.close(descriptor)
        partial.unlink(missing_ok=True)


if __name__ == '__main__':
    try:
        url, size, digest = artifact(os.environ['GITHUB_TOKEN'], os.environ['GITHUB_REPOSITORY'], os.environ['GITHUB_RUN_ID'])
        download(url, size, digest, Path(os.environ['BUNDLE_DIR']) / 'bundle.zip')
        print(f'Verified artifact download: {size} bytes')
    except Exception as error:
        # URL-bearing exceptions can contain short-lived artifact access tokens.
        print(f'Artifact transfer failed ({type(error).__name__}); use the offline bundle fallback.')
        raise SystemExit(1)
