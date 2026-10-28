#!/usr/bin/env python3
"""Real HTTP authentication tests for an isolated database/backend.

Use a test-only access-token lifetime of four seconds. No model calls.
"""
import argparse
import base64
import concurrent.futures
import json
import os
import subprocess
import time
import urllib.error
import urllib.request


class Client:
    def __init__(self, base):
        self.base = base
        self.token = ''
        self.refresh_token = ''

    def raw(self, method, path, body=None, token=None):
        headers = {'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream',
                   'Authorization': 'Bearer ' + (self.token if token is None else token)}
        req = urllib.request.Request(self.base + path, method=method, headers=headers,
                                     data=None if body is None else json.dumps(body).encode())
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            with opener.open(req, timeout=15) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)

    def renew(self):
        status, body = self.raw('POST', '/api/auth/refresh', {'refresh_token': self.refresh_token})
        assert status == 200, body
        self.token, self.refresh_token = body['token'], body['refresh_token']
        return body

    def call(self, method, path, body=None):
        status, data = self.raw(method, path, body)
        if status == 401 and self.refresh_token:
            self.renew()
            status, data = self.raw(method, path, body)
        return status, data

    def login(self, username, password):
        status, body = self.raw('POST', '/api/auth/login', {'username': username, 'password': password})
        assert status == 200, body
        self.token, self.refresh_token = body['token'], body['refresh_token']
        return body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-url', default='http://127.0.0.1:19028')
    parser.add_argument('--mysql-container', help='Optional isolated MySQL container for forced-expiry testing')
    args = parser.parse_args()
    admin = Client(args.base_url)
    admin.login('admin', os.environ['EMOS_TEST_PASSWORD'])
    temporary = '12345678'
    permanent = 'abcdefghijklmnop'
    reset = '11111111'
    status, created = admin.call('POST', '/api/users', {'username': 'auth-session-fixture', 'password': temporary, 'tenant_id': 1, 'daily_token_quota': 234567})
    assert status == 200 and not created['must_change_password'], created
    for invalid in ('1234567', 'abcdefghijklmnopq'):
        assert admin.call('POST', '/api/users', {'username': 'invalid-length-fixture', 'password': invalid, 'tenant_id': 1})[0] == 400
    uid = created['id']
    member = Client(args.base_url)
    assert not member.login('auth-session-fixture', temporary)['user']['must_change_password']
    assert member.raw('GET', '/api/documents')[0] == 200
    assert member.raw('GET', '/api/auth/me')[0] == 200
    assert member.raw('POST', '/api/auth/password', {'current_password': 'incorrect', 'new_password': permanent})[0] == 403
    old_access, old_refresh = member.token, member.refresh_token
    assert member.raw('POST', '/api/auth/password', {'current_password': temporary, 'new_password': permanent})[0] == 200
    assert member.raw('GET', '/api/auth/me', token=old_access)[0] == 401
    assert member.raw('POST', '/api/auth/refresh', {'refresh_token': old_refresh})[0] == 401
    assert member.raw('POST', '/api/auth/login', {'username': 'auth-session-fixture', 'password': temporary})[0] == 401
    assert not member.login('auth-session-fixture', permanent)['user']['must_change_password']
    assert member.raw('GET', '/api/documents')[0] == 200
    assert member.raw('POST', '/api/auth/password', {'current_password': permanent, 'new_password': permanent})[0] == 200
    member.login('auth-session-fixture', permanent)
    assert member.raw('POST', f'/api/users/{uid}/password', {'password': reset})[0] == 403
    assert admin.call('POST', f'/api/users/{uid}/password', {'password': 'short'})[0] == 400
    status, users = admin.call('GET', '/api/users')
    assert status == 200 and next(u for u in users['users'] if u['id'] == uid)['daily_token_quota'] == 234567

    stale_refresh = member.refresh_token
    member.renew()
    assert member.raw('POST', '/api/auth/refresh', {'refresh_token': stale_refresh})[0] == 401
    same_refresh = member.refresh_token
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        outcomes = list(pool.map(lambda _: member.raw('POST', '/api/auth/refresh', {'refresh_token': same_refresh}), range(2)))
    assert sorted(status for status, _ in outcomes) == [200, 401], outcomes
    fresh = next(body for status, body in outcomes if status == 200)
    member.token, member.refresh_token = fresh['token'], fresh['refresh_token']
    time.sleep(4.2)
    assert member.raw('GET', '/api/auth/me')[0] == 401
    member.renew()
    assert member.raw('GET', '/api/auth/me')[0] == 200

    # Logout must revoke the session even if another request rotates its secret.
    access, refresh = member.token, member.refresh_token
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        renewal = pool.submit(member.raw, 'POST', '/api/auth/refresh', {'refresh_token': refresh}, access)
        logout = pool.submit(member.raw, 'POST', '/api/auth/logout', {'refresh_token': refresh}, access)
        renewed, logged_out = renewal.result(), logout.result()
    assert logged_out[0] == 200 and renewed[0] in (200, 401), (logged_out, renewed)
    assert member.raw('GET', '/api/auth/me', token=access)[0] == 401
    if renewed[0] == 200:
        assert member.raw('GET', '/api/auth/me', token=renewed[1]['token'])[0] == 401
        assert member.raw('POST', '/api/auth/refresh', {'refresh_token': renewed[1]['refresh_token']})[0] == 401
    member.login('auth-session-fixture', permanent)

    before_reset = member.refresh_token
    rpc = {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/list', 'params': {}}
    assert member.raw('POST', '/mcp/knowledge', rpc)[0] == 200
    assert admin.call('POST', f'/api/users/{uid}/password', {'password': reset})[0] == 200
    assert member.raw('GET', '/api/auth/me')[0] == 401
    assert member.raw('POST', '/mcp/knowledge', rpc)[0] == 401
    assert member.raw('POST', '/api/auth/refresh', {'refresh_token': before_reset})[0] == 401
    assert not member.login('auth-session-fixture', reset)['user']['must_change_password']
    assert member.raw('POST', '/api/auth/password', {'current_password': reset, 'new_password': permanent})[0] == 200
    member.login('auth-session-fixture', permanent)
    assert member.raw('POST', '/api/auth/logout', {'refresh_token': member.refresh_token})[0] == 200
    assert member.raw('GET', '/api/auth/me')[0] == 401
    assert member.raw('POST', '/api/auth/refresh', {'refresh_token': member.refresh_token})[0] == 401
    member.login('auth-session-fixture', permanent)
    assert admin.call('DELETE', f'/api/users/{uid}')[0] == 200
    assert member.raw('GET', '/api/auth/me')[0] == 401
    assert member.raw('POST', '/api/auth/refresh', {'refresh_token': member.refresh_token})[0] == 401
    status, guest = admin.raw('POST', '/api/auth/guest', {})
    assert status == 200 and 'refresh_token' not in guest
    assert admin.raw('POST', '/api/auth/password', {'current_password': temporary, 'new_password': permanent}, token=guest['token'])[0] == 403
    if args.mysql_container:
        expired = Client(args.base_url)
        expired.login('admin', os.environ['EMOS_TEST_PASSWORD'])
        segment = expired.token.split('.')[1]
        claims = json.loads(base64.urlsafe_b64decode(segment + '=' * (-len(segment) % 4)))
        sid = claims['sid']
        assert len(sid) == 32 and all(c in '0123456789abcdef' for c in sid)
        subprocess.run(['docker', 'exec', '-i', args.mysql_container, 'mysql', '-uroot', 'llm_platform'],
                       input=f"UPDATE auth_sessions SET expires_at=TIMESTAMPADD(SECOND,-1,UTC_TIMESTAMP()) WHERE id='{sid}';",
                       text=True, check=True, capture_output=True)
        assert expired.raw('GET', '/api/auth/me')[0] == 401
        assert expired.raw('POST', '/api/auth/refresh', {'refresh_token': expired.refresh_token})[0] == 401
    print('PASS: optional password change, weak-password acceptance, self-change, admin reset, version revocation, actual JWT expiry/renewal, refresh rotation/replay/concurrency, logout, deleted users, guest restriction and quota preservation')


if __name__ == '__main__':
    main()
