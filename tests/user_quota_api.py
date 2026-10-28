#!/usr/bin/env python3
"""Exercise quota management against a local server without calling a model.

Requires an admin EMOS_EVAL_TOKEN. Creates and deletes one temporary user.
"""
import argparse
import json
import os
import secrets
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-url', default='http://127.0.0.1:3002')
    args = parser.parse_args()
    token = os.environ['EMOS_EVAL_TOKEN']
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(method, path, body=None, auth=token):
        headers = {'Authorization': 'Bearer ' + auth, 'Content-Type': 'application/json'}
        req = urllib.request.Request(args.base_url + path, method=method, headers=headers,
                                     data=None if body is None else json.dumps(body).encode())
        try:
            with opener.open(req, timeout=20) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)

    status, tenants = request('GET', '/api/tenants')
    assert status == 200
    tenant = tenants['tenants'][0]
    username = 'quota-api-' + secrets.token_hex(8)
    password = secrets.token_hex(8)
    user_id = None
    try:
        status, created = request('POST', '/api/users', {
            'username': username, 'password': password, 'tenant_id': tenant['id'],
            'role': 'user', 'daily_token_quota': 3000000000, 'must_change_password': False,
        })
        assert status == 200, created
        user_id = created['id']
        endpoint = f'/api/users/{user_id}/quota'

        def summary():
            status, response = request('GET', '/api/users')
            assert status == 200
            return next(user for user in response['users'] if user['id'] == user_id)

        assert summary()['effective_daily_token_quota'] == 3000000000
        for value in (0, -1, 1.5, '1000', True, [], {}, 9007199254740992, 18446744073709551615):
            assert request('POST', endpoint, {'daily_token_quota': value})[0] == 400, value
        assert request('POST', endpoint, {})[0] == 400
        assert request('POST', endpoint, [1])[0] == 400
        assert request('POST', '/api/users/2147483647/quota', {'daily_token_quota': 1})[0] == 404
        for value in (9007199254740991, 1, None):
            assert request('POST', endpoint, {'daily_token_quota': value})[0] == 200
            user = summary()
            expected = tenant['token_limit_daily'] if value is None else value
            assert user['daily_token_quota'] == value
            assert user['effective_daily_token_quota'] == user['today_token_limit'] == expected
            assert user['today_tokens_used'] == 0

        status, login = request('POST', '/api/auth/login', {'username': username, 'password': password}, auth='')
        assert status == 200
        assert request('POST', endpoint, {'daily_token_quota': 1}, auth=login['token'])[0] == 403
        assert request('GET', '/api/users', auth=login['token'])[0] == 403
        print('PASS: API creation, integer validation, max quota, immediate changes, inheritance, 404 and user authorization')
    finally:
        if user_id is not None:
            assert request('DELETE', f'/api/users/{user_id}')[0] == 200


if __name__ == '__main__':
    main()
