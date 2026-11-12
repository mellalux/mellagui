#!/usr/bin/env python3
"""A stand-in for the Smart Life / Tuya "device sharing" cloud that mellagui-smart talks to (tests).

    fake_tuya.py STATE.json [LOG]      serves on 127.0.0.1, prints "port N" and runs until it is killed

STATE.json: {"user_code": "abc123", "scan_after": 1, "expire": 7200, "username": "me@example.com",
             "homes": [{"ownerId": 1, "name": "Home"}],
             "devices": [{"id": "d1", "home": 1, "name": "Plug", "category": "cz", "online": true, "status": {"switch_1": false},
                          "functions": [{"code": "switch_1", "type": "Boolean", "values": "{}"}], "status_spec": [...]}]}
The file is read at every request and written back after a command, so a test can change it and see what a command did.
The login: the code is "scanned" after `scan_after` polls. Requests are checked like the real thing: the signature, the token, AES-GCM.
LOG gets one line per request: METHOD PATH [decrypted parameters or body].
"""
import base64, hashlib, hmac, json, os, sys, time, uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

STATE, LOG = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else None)
sessions = {}       # access token -> {'refresh': ..., 'until': time}
logins = {}         # qr token -> polls so far


def load():
    with open(STATE) as f:
        return json.load(f)


def save(st):
    tmp = STATE + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(st, f)
    os.replace(tmp, STATE)


def log(line):
    if LOG:
        with open(LOG, 'a') as f:
            f.write(line + '\n')


def secret_of(rid, refresh):
    hash_key = hashlib.md5((rid + refresh).encode()).hexdigest()
    return hash_key, hmac.new(rid.encode(), hash_key.encode(), hashlib.sha256).hexdigest()[:16]


def decrypt(text, secret):
    raw = base64.b64decode(text[:16]) + base64.b64decode(text[16:])     # base64(nonce) + base64(ciphertext)
    return AESGCM(secret.encode()).decrypt(raw[:12], raw[12:], None).decode()


def encrypt(text, secret):
    nonce = os.urandom(12)
    return base64.b64encode(nonce + AESGCM(secret.encode()).encrypt(nonce, text.encode(), None)).decode()


def new_session(st):
    access, refresh = uuid.uuid4().hex, uuid.uuid4().hex
    sessions[access] = {'refresh': refresh, 'until': time.time() + st.get('expire', 7200)}
    return access, refresh


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, obj):
        obj.setdefault('t', int(time.time() * 1000))
        data = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def fail(self, code, msg):
        self.reply({'success': False, 'code': code, 'msg': msg})

    def handle_any(self, method):
        st = load()
        url = urlparse(self.path)
        q = parse_qs(url.query)
        n = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(n).decode() if n else ''
        path = url.path
        base = '/v1.0/m/life/home-assistant/qrcode/tokens'
        if path == base and method == 'POST':
            log('POST %s usercode=%s' % (path, q.get('usercode', [''])[0]))
            if q.get('clientid', [''])[0] != 'HA_3y9q4ak7g4ephrvke' or q.get('schema', [''])[0] != 'haauthorize':
                return self.fail('1001', 'unknown client')
            if q.get('usercode', [''])[0] != st.get('user_code', 'abc123'):
                return self.fail('E0020001', 'the user code is not known')
            token = uuid.uuid4().hex
            logins[token] = 0
            return self.reply({'success': True, 'result': {'qrcode': token}})
        if path.startswith(base + '/') and method == 'GET':
            token = path[len(base) + 1:]
            if token not in logins:
                return self.fail('E0020002', 'the code has expired')
            logins[token] += 1
            if logins[token] <= st.get('scan_after', 1):
                return self.fail('E0020003', 'not scanned yet')
            access, refresh = new_session(st)
            log('LOGIN %s' % token)
            return self.reply({'success': True, 'result': {
                'access_token': access, 'refresh_token': refresh, 'expire_time': st.get('expire', 7200), 'uid': 'uid1',
                'username': st.get('username', 'me@example.com'), 'terminal_id': 'term1',
                'endpoint': 'http://127.0.0.1:%d' % self.server.server_address[1]}})

        # ---- the encrypted part
        h = self.headers
        rid, t, token, sign = h.get('X-requestId', ''), h.get('X-time', ''), h.get('X-token', ''), h.get('X-sign', '')
        sess = sessions.get(token)
        if not sess:
            log('%s %s REFUSED no session' % (method, path))
            return self.fail('1010', 'token invalid')
        hash_key, secret = secret_of(rid, sess['refresh'])
        qenc = q.get('encdata', [''])[0]
        benc = json.loads(body).get('encdata', '') if body else ''
        text = 'X-appKey=HA_3y9q4ak7g4ephrvke||X-requestId=%s||X-time=%s||X-token=%s%s%s' % (rid, t, token, qenc, benc)
        if hmac.new(hash_key.encode(), text.encode(), hashlib.sha256).hexdigest() != sign:
            log('%s %s REFUSED sign' % (method, path))
            return self.fail('1004', 'sign invalid')
        try:
            params = json.loads(decrypt(qenc, secret)) if qenc else {}
            data = json.loads(decrypt(benc, secret)) if benc else {}
        except Exception as e:
            log('%s %s REFUSED decrypt %r' % (method, path, e))
            return self.fail('1100', 'param is illegal')
        log('%s %s %s' % (method, path, json.dumps(params or data, sort_keys=True) if (params or data) else ''))

        def ok(result):
            self.reply({'success': True, 'result': encrypt(json.dumps(result), secret)})

        if path.startswith('/v1.0/m/token/') and method == 'GET':
            if path.split('/')[-1] != sess['refresh']:
                return self.fail('1010', 'token invalid')
            del sessions[token]
            access, refresh = new_session(st)
            return ok({'accessToken': access, 'refreshToken': refresh, 'expireTime': st.get('expire', 7200), 'uid': 'uid1'})
        if sess['until'] < time.time():
            return self.fail('1010', 'token is expired')
        if path == '/v1.0/m/life/users/homes':
            return ok(st.get('homes', [{'ownerId': 1, 'name': 'Home'}]))
        if path == '/v1.0/m/life/ha/home/devices':
            out = []
            for d in st.get('devices', []):
                if str(d.get('home', 1)) != str(params.get('homeId')):
                    continue
                out.append({'id': d['id'], 'name': d['name'], 'category': d.get('category', ''), 'product_name': d.get('product', ''),
                            'online': d.get('online', True), 'local_key': d.get('local_key', 'key-' + d['id']), 'ip': '192.0.2.7',
                            'status': [{'code': k, 'value': v} for k, v in d.get('status', {}).items()]})
            return ok(out)
        if path == '/v1.0/m/scene/ha/home/scenes':                         # "scenes": [{"scene_id", "name", "home", "enabled"}]
            return ok([{'scene_id': x['scene_id'], 'name': x['name'], 'enabled': x.get('enabled', True), 'actions': []}
                       for x in st.get('scenes', []) if str(x.get('home', 1)) == str(params.get('homeId'))])
        if path == '/v1.0/m/scene/ha/trigger' and method == 'POST':
            if not any(x['scene_id'] == data.get('sceneId') and str(x.get('home', 1)) == str(data.get('homeId')) for x in st.get('scenes', [])):
                return self.fail('2010', 'no such scene')
            return ok(True)
        parts = path.split('/')
        dev = next((d for d in st.get('devices', []) if d['id'] in parts), None)
        if path.endswith('/status') and '/devices/' in path and dev:        # the numbers of the local protocol: "dp_ids": {code: number}
            conv = dev.get('convert', {})
            return ok({'productKey': 'pk', 'dpStatusRelationDTOS': [
                {'dpId': n, 'statusCode': code, 'supportLocal': True, 'valueConvert': conv.get(code, 'default'), 'statusFormat': '{}',
                 'valueDesc': '{}', 'valueType': 'value', 'enumMappingMap': {}} for code, n in dev.get('dp_ids', {}).items()]})
        if path.endswith('/specifications') and dev:
            return ok({'category': dev.get('category', ''), 'functions': dev.get('functions', []), 'status': dev.get('status_spec', [])})
        if path.endswith('/commands') and dev and method == 'POST':
            if not dev.get('online', True):
                return self.fail('2001', 'device is offline')
            for c in data.get('commands', []):
                dev.setdefault('status', {})[c['code']] = c['value']
                if c['code'] == 'percent_control':
                    dev['status']['percent_state'] = c['value']
            save(st)
            return ok(True)
        return self.fail('1108', 'uri path invalid')

    def do_GET(self):
        self.handle_any('GET')

    def do_POST(self):
        self.handle_any('POST')


srv = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
print('port %d' % srv.server_address[1], flush=True)
srv.serve_forever()
