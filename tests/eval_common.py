"""Shared, dependency-free evaluation I/O. Tokens are read only from the environment."""
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import urllib.request
from urllib.error import HTTPError
from urllib.parse import urlsplit


class Client:
    def __init__(self, base_url, timeout=120):
        url = urlsplit(base_url)
        if url.scheme not in ('http', 'https') or not url.netloc or url.username or url.password or url.query or url.fragment:
            raise ValueError('base URL must be an HTTP(S) URL without credentials, query or fragment')
        self.base_url = base_url.rstrip('/')
        self.timeout = timeout
        token = os.environ.get('EMOS_EVAL_TOKEN', '')
        if not token:
            raise ValueError('Set EMOS_EVAL_TOKEN to a dedicated evaluation account JWT')
        self.headers = {'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'}
        # Do not forward bearer credentials through redirects.
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *args, **kwargs):
                return None
        self.opener = urllib.request.build_opener(NoRedirect)

    def request(self, path, payload=None, method='POST', headers=None):
        request = urllib.request.Request(self.base_url + path,
            data=json.dumps(payload).encode() if payload is not None else None,
            headers={**self.headers, **(headers or {})}, method=method)
        try:
            return self.opener.open(request, timeout=self.timeout)
        except HTTPError as error:
            error.close()
            raise

    def json(self, path, payload=None, method='POST', headers=None):
        with self.request(path, payload, method, headers) as response:
            return json.load(response)


def write_report(path, report):
    report["generated_at"] = datetime.now(timezone.utc).isoformat()
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(report['summary'], ensure_ascii=False, indent=2))
    print(f'Report: {target}')
