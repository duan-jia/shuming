"""Read-only release gate: selected image SHA must have green master checks."""
import json
import os
import re
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        return None


def require_success(payload, revision, events):
    runs = [run for run in payload['workflow_runs']
            if run.get('head_sha') == revision and run.get('head_branch') == 'master'
            and run.get('event') in events]
    if not runs:
        raise ValueError('No matching master validation')
    latest = max(runs, key=lambda run: run['id'])
    if latest.get('status') != 'completed' or latest.get('conclusion') != 'success':
        raise ValueError('Latest matching master validation has not passed')
    return latest['id']


def verify(revision, token, opener=None):
    if not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise ValueError('A full immutable image SHA is required')
    opener = opener or urllib.request.build_opener(NoRedirect())
    for workflow, events in [('ci.yml', {'push'}),
                             ('staging-e2e.yml', {'push', 'workflow_dispatch'})]:
        query = urllib.parse.urlencode({'head_sha': revision, 'branch': 'master', 'per_page': 100})
        url = f'https://api.github.com/repos/duan-jia/shuming/actions/workflows/{workflow}/runs?{query}'
        request = urllib.request.Request(url, headers={
            'Authorization': 'Bearer ' + token, 'Accept': 'application/vnd.github+json',
            'X-GitHub-Api-Version': '2022-11-28', 'User-Agent': 'shuming-release-gate'})
        with opener.open(request, timeout=15) as response:
            raw = response.read(4 * 1024 * 1024 + 1)
        if len(raw) > 4 * 1024 * 1024:
            raise ValueError('Oversized validation metadata')
        run = require_success(json.loads(raw), revision, events)
        print(f'Release gate passed: workflow={workflow} run={run}')


if __name__ == '__main__':
    try:
        verify(os.environ['IMAGE_REVISION'], os.environ['GITHUB_TOKEN'])
    except Exception:
        # HTTP errors can contain credentials or untrusted response bodies.
        raise SystemExit('Release blocked: selected SHA needs successful master CI and Staging E2E; validation lookup must succeed.') from None
