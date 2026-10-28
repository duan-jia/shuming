"""Versioned, content-addressed PDF text artifacts shared by upload and chat.

The database stores document/message references; full source text stays here.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

VERSION = 'poppler-layout-v1'
MAX_TEXT_BYTES = 4 * 1024 * 1024


def extract_pages(source, cache_directory, pass_fds=()):
    digest = hashlib.sha256()
    with open(source, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    sha = digest.hexdigest()
    directory = Path(cache_directory)
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / f'{sha}.{VERSION}.pages.json'
    # Concurrent uploads/readers share the same extraction. The OS releases
    # the lock even if the bounded parent process cancels a reader.
    with open(directory / f'{sha}.{VERSION}.lock', 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if target.exists():
            try:
                if target.stat().st_size > 8 * 1024 * 1024:
                    raise ValueError('Oversized PDF text artifact')
                result = json.loads(target.read_text(encoding='utf-8'))
                if result['source_sha256'] == sha and result['extractor_version'] == VERSION:
                    return result
            except (OSError, ValueError, KeyError):
                pass  # A damaged cache can be regenerated from the original.

        def run(*args):
            return subprocess.run(args, check=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, timeout=20,
                                  pass_fds=pass_fds).stdout

        info = run('pdfinfo', str(source)).decode('utf-8', errors='replace')
        match = re.search(r'^Pages:\s+(\d+)', info, re.MULTILINE)
        if not match or not 1 <= int(match[1]) <= 1000:
            raise ValueError('PDF must contain 1 to 1000 pages')
        count = int(match[1])
        raw = run('pdftotext', '-layout', '-enc', 'UTF-8', str(source), '-')
        if len(raw) > MAX_TEXT_BYTES:
            raise ValueError('Extracted PDF text exceeds 4 MiB')
        text = raw.decode('utf-8', errors='replace')
        pages = text.split('\f')
        if len(pages) == count + 1 and not pages[-1].strip():
            pages.pop()
        if len(pages) != count:
            raise ValueError('PDF page boundaries could not be preserved')
        # NUL/C0 bytes cannot safely pass through downstream database/JSON APIs.
        pages = [re.sub(r'[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]', ' ', page).strip() for page in pages]
        result = {'source_sha256': sha, 'extractor_version': VERSION,
                  'page_count': count, 'text_chars': sum(map(len, pages)),
                  'pages': [{'page': n + 1, 'text': text} for n, text in enumerate(pages)]}
        fd, temporary = tempfile.mkstemp(prefix='.pdf-text-', dir=directory)
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as stream:
                json.dump(result, stream, ensure_ascii=False)
            os.replace(temporary, target)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
        return result
