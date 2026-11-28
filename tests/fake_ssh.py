#!/usr/bin/env python3
"""A stand-in for `ssh -s ... sftp` ($MELLAGUI_SSH): the SFTP protocol version 3, as much as playing a file needs (init, open, fstat, read, close).

The folder $FAKE_SFTP_ROOT is the root of the "server". $FAKE_SFTP_PASS: a login without that password (asked through $SSH_ASKPASS, as ssh does) fails.
$FAKE_SSH_LOG: the arguments of every start and every file opened are appended there.
"""
import os
import struct
import subprocess
import sys

root = os.environ.get('FAKE_SFTP_ROOT', '')


def log(text):
    if os.environ.get('FAKE_SSH_LOG'):
        with open(os.environ['FAKE_SSH_LOG'], 'a') as f:
            f.write(text + '\n')


log('args ' + ' '.join(sys.argv[1:]))
want = os.environ.get('FAKE_SFTP_PASS')
if want is not None:
    ask = os.environ.get('SSH_ASKPASS')
    got = subprocess.run([ask, 'password:'], capture_output=True, text=True, timeout=10).stdout.rstrip('\n') if ask else ''
    if got != want:
        sys.stderr.write('me@host: Permission denied (publickey,password).\n')
        sys.exit(255)
    log('password ok')


def read(n):
    data = b''
    while len(data) < n:
        x = os.read(0, n - len(data))
        if not x:
            sys.exit(0)
        data += x
    return data


def send(kind, body):
    data = struct.pack('>IB', len(body) + 1, kind) + body
    while data:
        data = data[os.write(1, data):]


def status(ident, code):
    send(101, struct.pack('>III', ident, code, 0) + struct.pack('>I', 0))


files = {}
while True:
    n, kind = struct.unpack('>IB', read(5))
    body = read(n - 1)
    if kind == 1:
        send(2, struct.pack('>I', 3))
        continue
    ident = struct.unpack('>I', body[:4])[0]
    length = struct.unpack('>I', body[4:8])[0]
    arg = body[8:8 + length]
    if kind == 3:
        path = os.path.normpath('/' + arg.decode().lstrip('/'))
        log('open ' + path)
        try:
            h = b'h%d' % len(files)
            files[h] = open(os.path.join(root, path.lstrip('/')), 'rb')
            send(102, struct.pack('>II', ident, len(h)) + h)
        except OSError:
            status(ident, 2)
    elif kind == 8:
        send(105, struct.pack('>IIQ', ident, 1, os.fstat(files[arg].fileno()).st_size))
    elif kind == 5:
        off, want_n = struct.unpack('>QI', body[8 + length:20 + length])
        data = os.pread(files[arg].fileno(), want_n, off)
        if data:
            send(103, struct.pack('>II', ident, len(data)) + data)
        else:
            status(ident, 1)
    elif kind == 4:
        files.pop(arg).close()
        status(ident, 0)
    else:
        status(ident, 8)
