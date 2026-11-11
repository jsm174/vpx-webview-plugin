#!/usr/bin/env python3
"""Smoke test: builds the demo table, plays it in VPX, presses flippers and checks what the plugin did.

  tools/smoke.py /path/to/VPinballX_BGFX.app [--keep]

It drives VPX through its MCP server (http://127.0.0.1:2114/mcp), so it needs a VPX build that has one, and reads
VPX's log. The plugin must already be installed in that VPX. With --keep, VPX is left running at the end.
"""
import json
import pathlib
import subprocess
import sys
import tempfile
import time
import urllib.request

REPO = pathlib.Path(__file__).resolve().parent.parent
MCP_URL = 'http://127.0.0.1:2114/mcp'
LOG = pathlib.Path.home() / 'Library/Application Support/VPinballX/10.8/vpinball.log'


class Mcp:
    def __init__(self):
        self.session = None
        self.call('initialize', {'protocolVersion': '2025-03-26', 'capabilities': {}, 'clientInfo': {'name': 'webview-smoke', 'version': '1'}})
        self.call('notifications/initialized', notify=True)

    def call(self, method, params=None, notify=False):
        body = {'jsonrpc': '2.0', 'method': method}
        if not notify:
            body['id'] = 1
        if params is not None:
            body['params'] = params
        headers = {'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream'}
        if self.session:
            headers['Mcp-Session-Id'] = self.session
        response = urllib.request.urlopen(urllib.request.Request(MCP_URL, json.dumps(body).encode(), headers), timeout=30)
        self.session = response.headers.get('Mcp-Session-Id') or self.session
        text = response.read().decode()
        if not text.strip():
            return None
        if text.lstrip().startswith('{'):
            return json.loads(text)
        return json.loads([line[5:] for line in text.splitlines() if line.startswith('data:')][-1])

    def tool(self, name, **arguments):
        return self.call('tools/call', {'name': name, 'arguments': arguments})


class Log:
    def __init__(self):
        self.offset = LOG.stat().st_size if LOG.exists() else 0

    def text(self):
        if not LOG.exists():
            return ''
        data = LOG.read_bytes()
        if len(data) < self.offset:
            self.offset = 0
        return data[self.offset:].decode(errors='replace')

    def wait_for(self, needle, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            if needle in self.text():
                return True
            time.sleep(0.25)
        return False


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    keep = '--keep' in sys.argv
    if len(args) != 1:
        sys.exit(__doc__)
    out = pathlib.Path(tempfile.mkdtemp(prefix='webview-smoke-'))
    command = subprocess.run([sys.executable, str(REPO / 'tools/demo_table.py'), args[0], str(out)], check=True, capture_output=True, text=True).stdout.split()
    log = Log()
    vpx = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    results = []

    def check(name, ok, detail=''):
        results.append(ok)
        print(('PASS ' if ok else 'FAIL ') + name + (f' ({detail})' if detail else ''))

    try:
        check('CEF starts', log.wait_for('started (Chromium', 60))
        check('pages load from the table folder', log.wait_for('[logic] vpx://table/logic/index.html', 20))
        log.wait_for('Static PreRender done', 30)
        time.sleep(3)
        mcp = Mcp()
        mcp.tool('vpx_play')
        for _ in range(3):
            mcp.tool('vpx_input', action='LeftFlipper', holdMs=250)
            time.sleep(0.7)
        mcp.tool('vpx_input', action='RightFlipper', holdMs=100)
        check('logic page reads the table and reports back', log.wait_for("left flipper raised 3 times", 10))

        shot = out / 'screen1.png'
        mcp.tool('vpx_input', action='LeftFlipper', holdMs=100)
        time.sleep(0.3)
        mcp.tool('vpx_screenshot', target='dmd', link='ctrl://webview/display?id=1', format='png', path=str(shot))
        size = shot.stat().st_size if shot.exists() else 0
        check('screen 1 shows a picture', size > 20000, f'{size} bytes')

        mcp.tool('vpx_script', action='run', text='WebView.Open "outside", "https://example.com"')
        check('external urls are refused by default', log.wait_for('Refused https://example.com', 5))
        mcp.tool('vpx_script', action='run', text='WebView.Eval "fx", "fetch(\'https://example.com\').catch(() => 0)"')
        check('external requests from pages are blocked', log.wait_for('request to https://example.com/ blocked', 5))
        mcp.tool('vpx_script', action='run', text='WebView.Eval "fx", "fetch(\'vpx://table/..%2f..%2f..%2fetc%2fhosts\').then((r) => console.log(\'TRAVERSAL \' + r.status))"')
        check('urls cannot climb out of the table folder', log.wait_for('TRAVERSAL 404', 5))
        mcp.tool('vpx_script', action='run', text='WebView.Eval "fx", "vpx.table[\'gi1.State = 0 : x\'].State.catch((e) => console.log(\'INJECTION \' + e.message))"')
        check('names that carry code are rejected', log.wait_for('INJECTION Invalid name', 5))

        errors = [line for line in log.text().splitlines() if '[WebView:' in line and ' ERROR ' in line and 'Refused https://example.com' not in line]
        check('no plugin errors in the log', not errors, errors[0][-120:] if errors else '')

        if keep:
            print('VPX left running: ' + ' '.join(command))
            return
        try:
            mcp.tool('vpx_input', action='ExitGame', holdMs=100)
        except Exception:
            pass
        try:
            code = vpx.wait(timeout=20)
        except subprocess.TimeoutExpired:
            code = None
        check('VPX exits cleanly', code == 0, f'exit code {code}')
        time.sleep(1)
        helpers = subprocess.run(['pgrep', '-fl', 'webview/WebView Helper'], capture_output=True, text=True).stdout.strip()
        check('no helper processes left behind', not helpers)
    finally:
        if not keep and vpx.poll() is None:
            vpx.kill()
    print(f'{sum(results)}/{len(results)} checks passed')
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
