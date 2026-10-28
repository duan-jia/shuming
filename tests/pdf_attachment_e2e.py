#!/usr/bin/env python3
"""Full PDF upload/chat test against an isolated server + local mock model.

Never point this at production: the supplied provider log must contain only
synthetic requests. Requires EMOS_TEST_PASSWORD and a shared test data directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import urllib.error
import urllib.request

from native_mcp_e2e import fixture_pdf


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-url', default='http://127.0.0.1:13022')
    parser.add_argument('--fixture-directory', type=Path, required=True)
    args = parser.parse_args()
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    token = ''

    def request(method, path, body=None, auth=None, multipart=False):
        headers = {'Authorization': 'Bearer ' + (token if auth is None else auth)}
        if multipart:
            headers['Content-Type'] = 'multipart/form-data; boundary=AttachmentFixtureBoundary'
            payload = body
        else:
            headers['Content-Type'] = 'application/json'
            payload = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(args.base_url + path, method=method, headers=headers, data=payload)
        try:
            with opener.open(req, timeout=120) as response:
                data = response.read()
                return response.status, data if path.endswith('/stream') else json.loads(data)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)

    def stream(session, message, attachments, auth=None):
        status, body = request('POST', '/api/chat/stream', {
            'session_id': session, 'message': message, 'attachments': attachments,
        }, auth=auth)
        assert status == 200
        return [json.loads(line[6:]) for line in body.decode().splitlines()
                if line.startswith('data: ') and line != 'data: [DONE]']

    status, login = request('POST', '/api/auth/login', {'username': 'admin', 'password': os.environ['EMOS_TEST_PASSWORD']})
    assert status == 200, login
    token = login['token']
    source = fixture_pdf()
    args.fixture_directory.joinpath('fixture.pdf').write_bytes(source)
    boundary = b'--AttachmentFixtureBoundary\r\n'
    multipart = (boundary + b'Content-Disposition: form-data; name="visibility"\r\n\r\ngroup\r\n'
                 + boundary + b'Content-Disposition: form-data; name="file"; filename="fixture.pdf"\r\nContent-Type: application/pdf\r\n\r\n'
                 + source + b'\r\n--AttachmentFixtureBoundary--\r\n')
    status, uploaded = request('POST', '/api/papers/upload', multipart, multipart=True)
    assert status == 200, uploaded
    doc = uploaded['document_id']
    status, duplicate = request('POST', '/api/papers/upload', multipart, multipart=True)
    assert status == 409 and duplicate['document_id'] == doc, duplicate
    status, prepared = request('POST', '/api/chat/attachments', {'doc_id': doc})
    assert status == 200 and prepared['page_count'] == 4, prepared
    assert 'content' not in prepared
    sha = hashlib.sha256(source).hexdigest()
    cache = args.fixture_directory / 'data/processed/pdf-text' / f'{sha}.poppler-layout-v1.pages.json'
    artifact = json.loads(cache.read_text())
    assert 'row 59' in artifact['pages'][0]['text']
    assert 'page 4' in artifact['pages'][3]['text']
    mtime = cache.stat().st_mtime_ns
    request('POST', '/api/chat/attachments', {'doc_id': doc})
    assert cache.stat().st_mtime_ns == mtime

    _, session = request('POST', '/api/sessions', {'title': 'PDF attachment synthetic verification'})
    sid = session['id']
    events = stream(sid, 'Analyze the attached paper.', [doc])
    terminal = next(event for event in reversed(events) if event.get('type') == 'done')
    assert terminal['messages']['persisted'], events
    _, history = request('GET', '/api/sessions/' + sid)
    user = history['messages'][0]
    assert user['content'] == 'Analyze the attached paper.'
    assert user['attachments'][0]['doc_id'] == doc
    assert user['attachments'][0]['source_sha256'] == sha
    assert 'row 59' not in json.dumps(history)
    assert any(event.get('type') == 'done' for event in stream(sid, 'Continue analyzing page four.', []))
    events = stream(sid, 'Invalid attachment', ['1'])
    assert any('Invalid PDF attachment ID' in event.get('error', '') for event in events)

    _, tenant = request('POST', '/api/tenants', {'name': 'other-attachment-tenant', 'token_limit_daily': 100000})
    _, other = request('POST', '/api/users', {'username': 'other-attachment-user', 'password': 'other-pass-2026', 'tenant_id': tenant['id'], 'must_change_password': False})
    _, login = request('POST', '/api/auth/login', {'username': 'other-attachment-user', 'password': 'other-pass-2026'})
    other_token = login['token']
    assert request('POST', '/api/chat/attachments', {'doc_id': doc}, auth=other_token)[0] == 422
    assert request('POST', '/api/chat/attachments', {'doc_id': -1})[0] == 400
    assert request('POST', '/api/chat/attachments', {'doc_id': doc}, auth='invalid-token')[0] == 401
    _, other_session = request('POST', '/api/sessions', {'title': 'Other tenant'}, auth=other_token)
    assert any('not accessible' in event.get('error', '') for event in stream(other_session['id'], 'Read this', [doc], auth=other_token))

    requests = [json.loads(line) for line in args.fixture_directory.joinpath('requests.jsonl').read_text().splitlines()]
    cards = [item for item in requests if not item.get('stream')]
    chats = [item for item in requests if item.get('stream')]
    assert len(cards) == 1 and len(chats) == 2, (len(cards), len(chats))
    for chat in chats:
        prompt = chat['messages'][-1]['content']
        assert 'row 59' in prompt and 'page 4' in prompt and '[PDF page 4]' in prompt
        assert prompt.count('[PDF page 1]') == 1
    args.fixture_directory.joinpath('state.json').write_text(json.dumps({'doc_id': doc, 'session_id': sid, 'other_id': other['id']}))
    print('PASS: real PDF upload, dedup, disk cache reuse, complete text at mock provider, follow-up, attachment persistence, no full text in messages, tenant isolation and auth')


if __name__ == '__main__':
    main()
