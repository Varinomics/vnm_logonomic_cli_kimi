import argparse
import json
import os
from pathlib import Path
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

parser = argparse.ArgumentParser(description='Probe native Kimi model evidence using a loopback model and isolated profile.')
parser.add_argument('--cli', required=True, help='Official Kimi main.mjs path')
parser.add_argument('--helper', required=True, help='Built kimi_helper path')
parser.add_argument('--output-directory', required=True, help='New directory for isolated fixture data and logs')
args = parser.parse_args()
cli = Path(args.cli).resolve(strict=True)
helper = Path(args.helper).resolve(strict=True)
RUN = Path(args.output_directory).resolve()
RUN.mkdir(parents=True)
# Linux bubblewrap isolates the actual home mount without changing HOME or
# allowing native account/profile discovery. Only this new fixture is writable.
PROFILE = RUN / 'profile'
WORK = RUN / 'work'
for directory in [PROFILE, WORK, RUN / 'home', RUN / 'tmp']:
    directory.mkdir(exist_ok=True)
requests = []

class Endpoint(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['content-length'])))
        requests.append({'path': self.path, 'model': body.get('model')})
        assert body['model'] == 'native-root-model'
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        def emit(delta, finish=None):
            chunk = {'id': 'native-kimi-root', 'object': 'chat.completion.chunk',
                'created': 1, 'model': body['model'], 'choices': [
                    {'index': 0, 'delta': delta, 'finish_reason': finish}]}
            self.wfile.write(('data: ' + json.dumps(chunk) + '\n\n').encode())
        emit({'role': 'assistant', 'content': 'KIMI_NATIVE_MODEL_OK'})
        emit({}, 'stop')
        self.wfile.write(b'data: [DONE]\n\n')
        self.wfile.flush()

server = ThreadingHTTPServer(('127.0.0.1', 0), Endpoint)
threading.Thread(target=server.serve_forever, daemon=True).start()
wrapper = RUN / 'vnmctl'
wrapper.write_text('''#!/usr/bin/env python3
import json,os,pathlib,sys
if sys.argv[1:3] == ['provider-event','deliver']:
    value = json.load(sys.stdin)
    with open(os.environ['KIMI_TEST_EVENT_CAPTURE'], 'a') as output:
        output.write(json.dumps(value) + '\\n')
    print('{"accepted":true,"epoch":1}')
elif sys.argv[1:3] == ['provider-event','acknowledge']:
    pass
elif sys.argv[1] == 'request-title-instruction':
    print('{"accepted":true,"instruction_required":false,"instruction":""}')
else:
    sys.exit(2)
''')
wrapper.chmod(0o755)
(PROFILE / 'config.toml').write_text(f'''default_model = "scripted-alias"
builtin_product_skills = false
merge_all_available_skills = false
[providers.fixture]
type = "openai"
api_key = "fixture-only"
base_url = "http://127.0.0.1:{server.server_port}/v1"
[models.scripted-alias]
provider = "fixture"
model = "native-root-model"
max_context_size = 32768
capabilities = []
[model_catalog]
refresh_on_start = false
[workspace]
require_trust = false
[loop_control]
max_steps_per_turn = 2
max_attempts_per_step = 1
[[hooks]]
event = "SessionStart"
command = "{helper}"
timeout = 10
''')
capture = RUN / 'events.jsonl'
if capture.exists():
    capture.unlink()
environment = {key: os.environ[key] for key in ['PATH', 'LANG', 'LD_LIBRARY_PATH', 'HOME'] if key in os.environ}
environment.update({
    'KIMI_CODE_HOME': str(PROFILE), 'KIMI_DISABLE_TELEMETRY': '1',
    'KIMI_CODE_NO_AUTO_UPDATE': '1', 'KIMI_CODE_INFINITE_RETRY': '0',
    'KIMI_DISABLE_CRON': '1', 'KIMI_CODE_WATCH': '0',
    'HTTP_PROXY': 'http://127.0.0.1:9', 'HTTPS_PROXY': 'http://127.0.0.1:9',
    'ALL_PROXY': 'http://127.0.0.1:9', 'NO_PROXY': '127.0.0.1,localhost',
    'KIMI_TEST_EVENT_CAPTURE': str(capture),
    'LOGONOMIC_VNMCTL_PATH': str(wrapper),
    'LOGONOMIC_SESSION_CAPABILITY': 'a' * 64,
    'LOGONOMIC_PROVIDER_PACKAGE_ID': 'logonomic.cli.kimi',
    'LOGONOMIC_PROVIDER_GENERATION_DIGEST': 'b' * 64,
    'LOGONOMIC_PROVIDER_GENERATION_NONCE': 'c' * 32,
    'LOGONOMIC_PROVIDER_HELPERS_DIR': str(RUN / 'helpers'),
})
command = ['bwrap', '--die-with-parent', '--ro-bind', '/', '/', '--dev-bind', '/dev', '/dev', '--proc', '/proc',
    '--bind', str(RUN / 'home'), str(Path.home()), '--bind', str(RUN), str(RUN),
    '--bind', str(RUN / 'tmp'), '/tmp', '--chdir', str(WORK),
    'node', str(cli),
    '--prompt', 'Reply KIMI_NATIVE_MODEL_OK. Do not use tools.', '--output-format', 'stream-json']
try:
    result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=45)
    (RUN / 'stdout.jsonl').write_text(result.stdout)
    (RUN / 'stderr.txt').write_text(result.stderr)
    print(json.dumps({'exit': result.returncode, 'requests': requests,
        'stdout_tail': result.stdout[-2000:], 'stderr_tail': result.stderr[-2000:]}))
    assert result.returncode == 0
    assert requests
    facts = []
    usage_models = []
    for wire in PROFILE.glob('sessions/*/*/agents/main/wire.jsonl'):
        for line in wire.read_text().splitlines():
            record = json.loads(line)
            if record.get('type') == 'usage.record':
                usage_models.append(record.get('model'))
            if record.get('type') == 'llm.request':
                facts.append({key: record.get(key) for key in ['type', 'agentId', 'kind', 'provider', 'model', 'modelAlias']})
    supported = any(fact['model'] == 'native-root-model' and fact['agentId'] == 'main' and fact['kind'] == 'loop' for fact in facts)
    print(json.dumps({'native_model_records': facts, 'usage_models': usage_models,
        'helper_event_count': len(capture.read_text().splitlines()), 'root_model_supported': supported}))
    if not supported:
        raise SystemExit('Native root model evidence was not emitted; source cell remains unconfirmed.')
finally:
    server.shutdown()
