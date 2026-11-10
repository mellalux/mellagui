#!/usr/bin/env python3
"""A stand-in for smbclient in the tests: the folders under $FAKE_SMB_ROOT are the shares of the "server".

  FAKE_SMB_ROOT   the shares (one sub folder each)            FAKE_SMB_USER / FAKE_SMB_PASS   require this login (else guests are fine)
  FAKE_SMB_LOG    every call is appended here as one line     FAKE_SMB_SLOW   seconds to wait before answering (a dead router)
  FAKE_SMB_CHUNK  get / put move the file in 10 pieces, this many seconds apart (to watch a progress bar)
  FAKE_SMB_V1     answer "protocol negotiation failed" unless --option=client min protocol=NT1 is given
It understands what mellagui-files uses: -L //host, //host/share, -N, -A file, -D dir, --option and -c with ls / get / put / del / deltree / mkdir / rename.
"""
import os, shlex, shutil, sys, time

args = sys.argv[1:]
root = os.environ.get('FAKE_SMB_ROOT', '')
log = os.environ.get('FAKE_SMB_LOG')


def die(text, code=1):
    print(text)
    sys.exit(code)


target, listing, auth, dirname, cmd, legacy = None, False, None, '', None, False
i = 0
while i < len(args):
    a = args[i]
    if a == '-L':
        listing = True
        target = args[i + 1]
        i += 2
    elif a == '-N':
        i += 1
    elif a == '-A':
        auth = args[i + 1]
        i += 2
    elif a == '-D':
        dirname = args[i + 1]
        i += 2
    elif a == '-c':
        cmd = args[i + 1]
        i += 2
    elif a.startswith('--option='):
        legacy = legacy or 'NT1' in a
        i += 1
    elif a.startswith('//'):
        target = a
        i += 1
    else:
        i += 1

if log:
    with open(log, 'a') as f:
        f.write('%s | auth=%s | argv=%s\n' % (target, 'file' if auth else 'none', ' '.join(args)))
slow = float(os.environ.get('FAKE_SMB_SLOW', '0') or 0)
if slow:
    time.sleep(slow)
if os.environ.get('FAKE_SMB_V1') and not legacy:
    die('protocol negotiation failed: NT_STATUS_INVALID_NETWORK_RESPONSE')
user = os.environ.get('FAKE_SMB_USER')
if user:
    ok = False
    if auth and os.path.exists(auth):
        kv = dict((l.split('=', 1)[0].strip(), l.split('=', 1)[1].strip()) for l in open(auth) if '=' in l)
        ok = kv.get('username') == user and kv.get('password') == os.environ.get('FAKE_SMB_PASS', '')
    if not ok:
        die('session setup failed: NT_STATUS_LOGON_FAILURE')
if not target:
    die('usage', 2)
parts = target[2:].split('/', 1)
host, share = parts[0], parts[1] if len(parts) > 1 else ''
if listing:
    print('Anonymous login successful' if not user else '')
    print('\n\tSharename       Type      Comment\n\t---------       ----      -------')
    for s in sorted(os.listdir(root)):
        if os.path.isdir(os.path.join(root, s)):
            print('\t%-15s Disk      comment of %s' % (s, s))
    print('\tIPC$            IPC       IPC Service (FAKE)')
    print('SMB1 disabled -- no workgroup available')
    sys.exit(0)
base = os.path.join(root, share)
if not share or not os.path.isdir(base):
    die('tree connect failed: NT_STATUS_BAD_NETWORK_NAME')
cwd = os.path.join(base, dirname.replace('\\', '/')) if dirname else base
cwd = os.path.normpath(cwd)
if not cwd.startswith(os.path.normpath(base)) or not os.path.isdir(cwd):
    die('cd %s: NT_STATUS_OBJECT_PATH_NOT_FOUND' % dirname)


def inside(p):
    q = os.path.normpath(os.path.join(cwd, p.replace('\\', '/')))
    if not q.startswith(os.path.normpath(base)):
        die('NT_STATUS_ACCESS_DENIED')
    return q


def copy(src, dst):
    """shutil.copyfile, or in ten slow pieces with FAKE_SMB_CHUNK (reading the source with plain read(), as smbclient does)."""
    pause = float(os.environ.get('FAKE_SMB_CHUNK', '0') or 0)
    if not pause:
        shutil.copyfile(src, dst)
        return
    size = os.path.getsize(src)
    piece = max(size // 10, 1)
    with open(src, 'rb') as a, open(dst, 'wb') as b:
        while True:
            buf = a.read(piece)
            if not buf:
                break
            b.write(buf)
            b.flush()
            time.sleep(pause)


if cmd is None:
    sys.exit(0)
words = shlex.split(cmd)
verb, rest = words[0], words[1:]
if verb == 'ls':
    def line(name, d, size, mt):
        return '  %-35s %s %8d  %s' % (name, 'D' if d else 'N', size, time.strftime('%a %b %e %H:%M:%S %Y', time.localtime(mt)))
    now = time.time()
    print(line('.', True, 0, now))
    print(line('..', True, 0, now))
    for n in sorted(os.listdir(cwd)):
        p = os.path.join(cwd, n)
        st = os.stat(p)
        print(line(n, os.path.isdir(p), 0 if os.path.isdir(p) else st.st_size, st.st_mtime))
    print('\n\t\t974405520 blocks of size 1024. 12345 blocks available')
elif verb == 'get':
    src = inside(rest[0])
    if not os.path.isfile(src):
        die('NT_STATUS_OBJECT_NAME_NOT_FOUND opening remote file \\%s' % rest[0])
    copy(src, rest[1])
    print('getting file \\%s of size %d as %s' % (rest[0], os.path.getsize(src), rest[1]))
elif verb == 'put':
    if not os.path.isfile(rest[0]):
        die('%s does not exist' % rest[0])
    dst = inside(rest[1])
    if os.path.exists(dst) and os.environ.get('FAKE_SMB_READONLY'):
        die('NT_STATUS_ACCESS_DENIED')
    copy(rest[0], dst)
    print('putting file %s as \\%s' % (rest[0], rest[1]))
elif verb == 'del':
    p = inside(rest[0])
    if not os.path.isfile(p):
        die('NT_STATUS_NO_SUCH_FILE deleting remote file \\%s' % rest[0])
    os.unlink(p)
elif verb == 'deltree':
    p = inside(rest[0])
    if not os.path.exists(p):
        die('NT_STATUS_OBJECT_NAME_NOT_FOUND')
    shutil.rmtree(p) if os.path.isdir(p) else os.unlink(p)
elif verb == 'mkdir':
    p = inside(rest[0])
    if os.path.exists(p):
        die('NT_STATUS_OBJECT_NAME_COLLISION making remote directory \\%s' % rest[0])
    os.mkdir(p)
elif verb == 'rename':
    a, b = inside(rest[0]), inside(rest[1])
    if os.path.exists(b):
        die('NT_STATUS_OBJECT_NAME_COLLISION renaming files \\%s -> \\%s' % (rest[0], rest[1]))
    os.rename(a, b)
else:
    die('command not found: %s' % verb)
