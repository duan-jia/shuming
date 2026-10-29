"""Run inside the backend image as the deployment UID, without host mounts."""
import json
import os
import base64
from pathlib import Path
import subprocess
import shutil
import tempfile


def fixture_pdf():
    text = b'BT /F1 12 Tf 20 100 Td (SHUMING-STAGING-UID-CHECK) Tj ET'
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>',
        b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] '
        b'/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
        b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
        b'<< /Length ' + str(len(text)).encode() + b' >>\nstream\n' + text + b'\nendstream']
    output, offsets = b'%PDF-1.4\n', [0]
    for number, item in enumerate(objects, 1):
        offsets.append(len(output))
        output += str(number).encode() + b' 0 obj\n' + item + b'\nendobj\n'
    xref = len(output)
    output += b'xref\n0 6\n0000000000 65535 f \n'
    output += b''.join(f'{offset:010d} 00000 n \n'.encode() for offset in offsets[1:])
    return output + b'trailer << /Size 6 /Root 1 0 R >>\nstartxref\n' + str(xref).encode() + b'\n%%EOF\n'


if __name__ == '__main__':
    for script in ('read_pdf.py', 'pdf_text.py'):
        if not os.access('/app/scripts/' + script, os.R_OK):
            raise SystemExit('Deployment UID cannot read /app/scripts/' + script)
    for tool in ('pdfinfo', 'pdftotext', 'pdftoppm'):
        if not shutil.which(tool):
            raise SystemExit('Backend image is missing ' + tool)
    with tempfile.TemporaryDirectory(prefix='shuming-runtime-') as directory:
        source = Path(directory) / 'fixture.pdf'
        source.write_bytes(fixture_pdf())
        fd = os.open(source, os.O_RDONLY)
        os.dup2(fd, 3)
        try:
            result = subprocess.run(['python3', '/app/scripts/read_pdf.py', json.dumps({
                'representation': 'attachment', 'cache_directory': directory + '/cache'})],
                pass_fds=(3,), stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            if result.returncode:
                raise SystemExit('Backend image cannot read a synthetic PDF as the deployment UID')
            data = json.loads(result.stdout)
            if data.get('page_count') != 1 or 'SHUMING-STAGING-UID-CHECK' not in data.get('content', ''):
                raise SystemExit('Backend PDF reader returned incomplete synthetic text')
            rendered = subprocess.run(['python3', '/app/scripts/read_pdf.py', json.dumps({
                'representation': 'pages', 'start_page': 1, 'page_count': 1})],
                pass_fds=(3,), stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=True)
            image = json.loads(rendered.stdout)['images'][0]
            if not base64.b64decode(image['data']).startswith(b'\x89PNG\r\n\x1a\n'):
                raise SystemExit('Backend PDF page rendering failed')
        finally:
            os.close(fd)
            if fd != 3:
                os.close(3)
    print('Backend deployment UID, Python scripts, Poppler and PDF cache verified')
