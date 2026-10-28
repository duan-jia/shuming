#!/usr/bin/env python3
"""Bounded PDF reader for the backend. Poppler processes run inside its process group.

Input is an already-authorized inherited file descriptor, never a user-supplied path.
Outputs original PDF text or rendered page images, never generated paper-card text.
"""
import base64
import json
import re
import resource
import subprocess
import sys
import tempfile
from pathlib import Path
from pdf_text import extract_pages


def run(*args):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, timeout=15, pass_fds=(3,)).stdout


def main():
    resource.setrlimit(resource.RLIMIT_AS, (768 * 1024**2, 768 * 1024**2))
    resource.setrlimit(resource.RLIMIT_FSIZE, (8 * 1024**2, 8 * 1024**2))
    args = json.loads(sys.argv[1])
    source = '/proc/self/fd/3'
    if args.get('representation') == 'attachment':
        extracted = extract_pages(source, args['cache_directory'], pass_fds=(3,))
        if not extracted['text_chars']:
            raise ValueError('PDF has no extractable text')
        return {**{key: value for key, value in extracted.items() if key != 'pages'},
                'content': '\n\n'.join(f"[PDF page {page['page']}]\n{page['text']}" for page in extracted['pages'])}
    info = run('pdfinfo', source).decode('utf-8', errors='replace')
    match = re.search(r'^Pages:\s+(\d+)', info, re.MULTILINE)
    if not match:
        raise ValueError('Cannot determine PDF page count')
    total = int(match[1])
    start = args.get('start_page', 1)
    end = min(total, start + args.get('page_count', 3) - 1)
    if start > total:
        return {'error': 'start_page exceeds page count', 'total_pages': total}
    result = {'total_pages': total, 'start_page': start, 'end_page': end,
              'next_page': end + 1 if end < total else None}
    if args.get('representation', 'text') == 'text':
        pages = []
        for number in range(start, end + 1):
            text = run('pdftotext', '-f', str(number), '-l', str(number),
                       '-layout', '-enc', 'UTF-8', source, '-').decode('utf-8', errors='replace')
            pages.append(f'\n[PDF page {number}]\n{text}')
        text = ''.join(pages)
        offset = args.get('offset', 0)
        content = text[offset:offset + 4000]
        result.update(content=content, offset=offset,
                      next_offset=offset + len(content) if offset + len(content) < len(text) else None)
        # Scanned pages still remain readable through representation=pages.
        result['text_available'] = any(p.split(']\n', 1)[-1].strip('\f\n ') for p in pages)
        if offset > len(text):
            result['error'] = 'offset exceeds text length for this page range'
    else:
        images = []
        with tempfile.TemporaryDirectory(prefix='shuming-pdf-') as directory:
            for number in range(start, end + 1):
                stem = str(Path(directory) / f'page-{number}')
                run('pdftoppm', '-f', str(number), '-l', str(number), '-singlefile',
                    '-scale-to', '1600', '-png', source, stem)
                image = Path(stem + '.png').read_bytes()
                images.append({'type': 'image', 'mimeType': 'image/png',
                               'data': base64.b64encode(image).decode('ascii')})
        result['images'] = images
    return result


if __name__ == '__main__':
    try:
        print(json.dumps(main(), ensure_ascii=False))
    except Exception:
        # No PDF text, local paths, or subprocess diagnostics in error messages.
        print(json.dumps({'error': 'PDF processing failed; verify PDF validity and Poppler installation'}))
        sys.exit(1)
