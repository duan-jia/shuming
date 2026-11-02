#!/usr/bin/env python3
"""Export, inspect and replay Agent traces. Replays never execute tools.

Offline inspection is the default. Only `replay --live` sends recorded context
to the model endpoint explicitly selected by --model-config.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import sys
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_):
        return None


def api_base(value):
    parsed = urllib.parse.urlsplit(value)
    local = parsed.hostname in ('127.0.0.1', 'localhost', '::1')
    if (parsed.scheme != 'https' and not (parsed.scheme == 'http' and local)) or not parsed.hostname:
        raise ValueError('Use HTTPS, or HTTP on localhost, for the API endpoint')
    if parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError('API endpoints must not contain credentials, query strings or fragments')
    return value.rstrip('/')


def request_json(url, token='', body=None, timeout=120):
    headers = {'Accept': 'application/json', 'Content-Type': 'application/json'}
    if token:
        headers['Authorization'] = 'Bearer ' + token
    request = urllib.request.Request(url, headers=headers,
        data=None if body is None else json.dumps(body, ensure_ascii=False).encode())
    # Do not forward bearer credentials through redirects.
    opener = urllib.request.build_opener(NoRedirect())
    try:
        with opener.open(request, timeout=timeout) as response:
            data = response.read(16 * 1024 * 1024 + 1)
            if len(data) > 16 * 1024 * 1024:
                raise ValueError('API response exceeds the 16 MiB replay limit')
            return json.loads(data)
    except urllib.error.HTTPError as error:
        # Upstream error bodies can contain provider or credential information.
        raise ValueError(f'API returned HTTP {error.code}; no response body was exported') from None


def redact(value):
    if isinstance(value, dict):
        return {key: '[redacted]' if key.lower() in {
            'authorization', 'api_key', 'password', 'access_token', 'refresh_token', 'download_url', 'signing_secret'
        } else redact(item) for key, item in value.items()}
    if isinstance(value, list):
        return [redact(item) for item in value]
    if isinstance(value, str):
        if value.startswith('data:image/'):
            return '[image data omitted]'
        try:
            nested = json.loads(value)
        except (ValueError, TypeError):
            nested = None
        if isinstance(nested, (dict, list)):
            safe = redact(nested)
            if safe != nested:
                return json.dumps(safe, ensure_ascii=False)
        return re.sub(r'(/api/knowledge/download\?)[^\s"\x27<>)]*', r'\1[redacted]', value)
    return value


def output_directory(path):
    path = Path(path)
    if path.exists() and any(path.iterdir()):
        raise ValueError('Output directory must be new or empty; previous artifacts will not be overwritten')
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    path.chmod(0o700)
    return path


def write_json(path, value):
    # Credentials and signed links are never part of exported artifacts.
    with Path(path).open('x', encoding='utf-8') as target:
        os.chmod(path, 0o600)
        json.dump(redact(value), target, ensure_ascii=False, indent=2)
        target.write('\n')


def trace_requests(trace, require_requests=True):
    if not isinstance(trace, dict) or not isinstance(trace.get('events'), list):
        raise ValueError('Expected an exported trace object with an events array')
    requests = [e['data'] for e in trace['events'] if e.get('type') == 'model_request']
    if not requests and require_requests:
        raise ValueError('No model_request exists; legacy conversation rows cannot reconstruct a trace')
    for body in requests:
        if not isinstance(body, dict) or not isinstance(body.get('messages'), list):
            raise ValueError('Malformed model_request in trace')
    return requests


def check_messages(messages):
    pending = set()
    errors = []
    for index, message in enumerate(messages):
        role = message.get('role')
        if pending and role != 'tool':
            errors.append(f'message {index}: next message arrived before all tool results')
            pending.clear()
        if role == 'assistant':
            ids = [call['id'] for call in message.get('tool_calls', [])]
            if len(ids) != len(set(ids)):
                errors.append(f'message {index}: duplicate call IDs in one batch')
            pending.update(ids)
        elif role == 'tool':
            call_id = message.get('tool_call_id')
            if call_id not in pending:
                errors.append(f'message {index}: orphan tool result')
            pending.discard(call_id)
    if pending:
        errors.append('request ends with unanswered tool calls')
    return errors


def export_artifacts(trace, directory):
    requests = trace_requests(trace, require_requests=False)
    write_json(directory / 'trace.json', trace)
    report = ['# Agent trace', '', 'Request ID: ' + str(trace.get('request_id', 'unknown')), '',
              'These are redacted semantic records, not byte-identical HTTP captures.', '',
              '| Model turn | Message roles | Tool results | Pairing |', '|---|---|---:|---|']
    for index, body in enumerate(requests, 1):
        write_json(directory / f'model-request-{index:02d}.json', body)
        messages = body['messages']
        errors = check_messages(messages)
        report.append(f"| {index} | {' → '.join(m.get('role', '?') for m in messages)} | "
                      f"{sum(m.get('role') == 'tool' for m in messages)} | {'; '.join(errors) or 'OK'} |")
    report += ['', '## Tool execution', '']
    if not requests:
        report += ['No model HTTP request was recorded; inspect the events for early rejection or failure.', '']
    for event in trace['events']:
        data = event.get('data', {})
        if event.get('type') == 'tool_start':
            report += [f"- {data.get('tool_call_id')}: {data.get('name')}; arguments: "
                       + json.dumps(redact(data.get('arguments')), ensure_ascii=False)]
        elif event.get('type') == 'tool_end':
            report += [f"  - stage={data.get('stage')}, execution_started={data.get('execution_started')}, "
                       f"is_error={data.get('is_error')}, duration_ms={data.get('duration_ms')}"]
    report += ['', '## Recorded model responses', '']
    for event in trace['events']:
        if event.get('type') == 'model_response':
            report += [str(redact(event.get('data', {}).get('text', ''))), '']
    with (directory / 'report.md').open('x', encoding='utf-8') as target:
        os.chmod(target.name, 0o600)
        target.write('\n'.join(report) + '\n')
    return requests


def export_trace(args):
    base = api_base(args.base_url)
    token = os.environ.get(args.token_env, '')
    if not token and args.login_config:
        config = json.loads(Path(args.login_config).read_text())
        admin = config['admin']
        token = request_json(base + '/api/auth/login', body={
            'username': admin.get('username', 'admin'), 'password': admin['default_password']})['token']
    if not token:
        raise ValueError('Provide a token through --token-env or explicitly use --login-config')
    session = urllib.parse.quote(args.session_id, safe='')
    path = base + '/api/sessions/' + session + '/requests'
    request_id = args.request_id
    if not request_id:
        recent = request_json(path, token)['requests']
        if not recent:
            raise ValueError('This session has no recorded requests; legacy tool history cannot be recovered')
        request_id = recent[0]['request_id']
    trace = request_json(path + '/' + urllib.parse.quote(request_id, safe=''), token)
    directory = output_directory(args.output)
    requests = export_artifacts(trace, directory)
    print(f'Exported request {request_id}: {len(requests)} model turn(s) to {directory}')


def replay_body(recorded, max_tokens, without_tools=False):
    body = copy.deepcopy(recorded)
    if '[image data omitted' in json.dumps(body):
        raise ValueError('Page images were omitted from this trace; exact replay needs the original images')
    if check_messages(body['messages']):
        raise ValueError('Cannot replay a malformed tool call/result sequence')
    if without_tools:
        messages = []
        for message in body['messages']:
            if message.get('role') == 'tool':
                continue
            if message.get('tool_calls'):
                message.pop('tool_calls')
                message.pop('reasoning_content', None)
                if not message.get('content'):
                    continue
            messages.append(message)
        body['messages'] = messages
    # Freeze evidence: model generation only, never execute returned tool calls.
    body['stream'] = False
    body.pop('stream_options', None)
    body['max_tokens'] = max_tokens
    if body.get('tools'):
        body['tool_choice'] = 'none'
    return body


def replay(args):
    trace = json.loads(Path(args.trace).read_text())
    requests = trace_requests(trace)
    index = len(requests) - 1 if args.turn == 'last' else int(args.turn) - 1
    if index < 0 or index >= len(requests):
        raise ValueError('Model turn is outside the recorded request range')
    directory = output_directory(args.output)
    variants = [('with-tools', False)]
    if args.compare_without_tool_history:
        variants.append(('without-tool-history', True))
    config = json.loads(Path(args.model_config).read_text()) if args.live and args.model_config else None
    endpoint = key = None
    if args.live:
        if config is None:
            raise ValueError('--live requires --model-config; no endpoint is taken from the trace')
        remote = config['remote_engine']
        endpoint = api_base(os.environ.get('REMOTE_ENGINE_ENDPOINT') or remote['endpoint'])
        model = os.environ.get('REMOTE_ENGINE_MODEL') or remote['model']
        if requests[index].get('model') != model:
            raise ValueError('Recorded model differs from configured model; choose a matching config')
        key = os.environ.get('REMOTE_ENGINE_API_KEY') or remote.get('api_key') or os.environ.get('DEEPSEEK_API_KEY')
        if not key:
            raise ValueError('No model API key configured')
    report = ['# Fixed-evidence replay', '', f'Recorded model turn: {index + 1}', '',
              'Tools are never executed. New generation uses fixed recorded results.',
              'Replay sets stream=false, tool_choice=none and the requested output limit.',
              'Removing tool history is an ablation, not a reconstruction of a missing old trace.', '']
    for name, without_tools in variants:
        body = replay_body(requests[index], args.max_tokens, without_tools)
        write_json(directory / f'{name}.request.json', body)
        report += ['## ' + name, '', 'Message roles: ' + ' → '.join(m['role'] for m in body['messages']), '']
        if args.live:
            response = request_json(endpoint + '/chat/completions', key, body)
            write_json(directory / f'{name}.response.json', response)
            choices = response.get('choices', [])
            if not choices:
                raise ValueError('Model returned no choices; inspect the exported response')
            message = choices[0].get('message', {})
            report += [str(redact(message.get('content', ''))), '',
                       'Finish reason: ' + str(choices[0].get('finish_reason', 'not reported')), '',
                       'Reported model: ' + str(response.get('model', 'not reported')), '',
                       'Usage: ' + json.dumps(response.get('usage', {}), ensure_ascii=False), '']
            if message.get('tool_calls'):
                report += ['Model returned tool calls despite tool_choice=none; they were NOT executed.', '']
        else:
            report += ['Offline only: no model request was sent.', '']
    with (directory / 'replay.md').open('x', encoding='utf-8') as target:
        os.chmod(target.name, 0o600)
        target.write('\n'.join(report) + '\n')
    print(f"{'Live' if args.live else 'Offline'} replay artifacts saved to {directory}; no tools executed")


def main(argv=None):
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    export = commands.add_parser('export', help='Read an existing trace through authenticated local service APIs')
    export.add_argument('--base-url', default='http://127.0.0.1:3002')
    export.add_argument('--session-id', required=True)
    export.add_argument('--request-id', help='Defaults to the latest recorded request in this session')
    export.add_argument('--token-env', default='EMOS_TRACE_TOKEN')
    export.add_argument('--login-config', help='Explicit local app config for bootstrap admin login; credentials are never exported')
    export.add_argument('--output', required=True)
    inspect = commands.add_parser('inspect', help='Export readable artifacts from a local trace JSON without network calls')
    inspect.add_argument('trace')
    inspect.add_argument('--output', required=True)
    replay_parser = commands.add_parser('replay', help='Prepare replay JSON; --live explicitly sends context to the configured model')
    replay_parser.add_argument('trace')
    replay_parser.add_argument('--turn', default='last', help='1-based model turn, or last')
    replay_parser.add_argument('--output', required=True)
    replay_parser.add_argument('--model-config')
    replay_parser.add_argument('--live', action='store_true')
    replay_parser.add_argument('--max-tokens', type=int, default=2048)
    replay_parser.add_argument('--compare-without-tool-history', action='store_true')
    args = parser.parse_args(argv)
    try:
        if args.command == 'export':
            export_trace(args)
        elif args.command == 'inspect':
            directory = output_directory(args.output)
            requests = export_artifacts(json.loads(Path(args.trace).read_text()), directory)
            print(f'Inspected {len(requests)} model turn(s); no network calls')
        else:
            if not 1 <= args.max_tokens <= 65536:
                raise ValueError('max-tokens must be between 1 and 65536')
            replay(args)
    except (ValueError, KeyError, OSError, urllib.error.URLError) as error:
        print(f'Trace operation failed: {type(error).__name__}: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
