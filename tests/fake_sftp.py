#!/usr/bin/env python3
"""A stand-in for `sftp -b -` in the tests: the folder $FAKE_SFTP_ROOT is the root of the "server"; the home folder is /home/me in it.

  FAKE_SFTP_LOG           every call is appended as one line (arguments, and whether a password came through the environment)
  FAKE_SFTP_USER          the only user name the server knows (the login fails for another one)
  FAKE_SFTP_PASS          the password: a login without MELLAGUI_SFTP_PASS (as ssh's askpass would bring it) or with another one fails
  FAKE_SFTP_CHUNK         get / put move the file in ten pieces, this many seconds apart (to watch a progress bar)
  FAKE_SFTP_CHANGED       answer "REMOTE HOST IDENTIFICATION HAS CHANGED"
Understands: pwd, ls -la, get [-r], put [-r], rm, rmdir, mkdir, rename; the first command that fails ends the batch with status 1, like sftp -b.
"""
import os, shlex, shutil, sys, time

args = sys.argv[1:]
root = os.environ.get('FAKE_SFTP_ROOT', '')
log = os.environ.get('FAKE_SFTP_LOG')
target, port, opts = None, None, []
i = 0
while i < len(args):
    a = args[i]
    if a == '-b':
        i += 2
    elif a == '-o':
        opts.append(args[i + 1])
        i += 2
    elif a == '-P':
        port = args[i + 1]
        i += 2
    else:
        target = a
        i += 1
user, _, host = (target or '').rpartition('@')
password = os.environ.get('MELLAGUI_SFTP_PASS', '')
if log:
    with open(log, 'a') as f:
        f.write('%s | user=%s port=%s pass=%s batch=%s | %s\n' % (host, user, port, 'env' if password else 'none',
                'BatchMode=yes' in opts, ' '.join(args)))
if os.environ.get('FAKE_SFTP_CHANGED'):
    print('@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@')
    print('@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @')
    sys.exit(255)
want_user = os.environ.get('FAKE_SFTP_USER')
want_pass = os.environ.get('FAKE_SFTP_PASS')
if (want_user and user != want_user) or (want_pass is not None and password != want_pass):
    print('%s: Permission denied (publickey,password).' % target)
    sys.exit(255)
os.makedirs(os.path.join(root, 'home', 'me'), exist_ok=True)
print('Connected to %s.' % host)
home = '/home/me'


def real(p):
    p = os.path.normpath('/' + p.lstrip('/'))
    return os.path.join(root, p.lstrip('/'))


def copy(src, dst):
    pause = float(os.environ.get('FAKE_SFTP_CHUNK', '0') or 0)
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


def fail(text):
    print(text)
    sys.exit(1)


def lsline(name, path):
    st = os.lstat(path)
    mode = 'd' if os.path.isdir(path) else '-'
    t = time.strftime('%b %e %H:%M', time.localtime(st.st_mtime))
    return '%srwxr-xr-x    1 me       me %12d %s %s' % (mode, 0 if mode == 'd' else st.st_size, t, name)


for line in sys.stdin.read().splitlines():
    if not line.strip():
        continue
    print('sftp> ' + line)
    w = shlex.split(line)
    cmd, rest = w[0], w[1:]
    if cmd == 'pwd':
        print('Remote working directory: ' + home)
    elif cmd == 'ls':
        paths = [x for x in rest if not x.startswith('-')]
        d = real(paths[0] if paths else home)
        if not os.path.isdir(d):
            fail('Can\'t ls: "%s" not found' % (paths[0] if paths else home))
        print(lsline('.', d))
        print(lsline('..', d))
        for n in sorted(os.listdir(d)):
            print(lsline(n, os.path.join(d, n)))
    elif cmd == 'get':
        rec = '-r' in rest
        a = [x for x in rest if not x.startswith('-')]
        src, dst = real(a[0]), a[1]
        if not os.path.exists(src):
            fail('File "%s" not found.' % a[0])
        if rec and os.path.isdir(src):
            shutil.copytree(src, dst, dirs_exist_ok=True)
        else:
            if os.path.isdir(dst):
                dst = os.path.join(dst, os.path.basename(src))
            copy(src, dst)
    elif cmd == 'put':
        rec = '-r' in rest
        a = [x for x in rest if not x.startswith('-')]
        src, dst = a[0], real(a[1])
        if not os.path.exists(src):
            fail('%s: No such file or directory' % src)
        if os.path.isdir(src):
            if not rec:
                fail('%s is not a regular file' % src)
            shutil.copytree(src, dst)
        else:
            if not os.path.isdir(os.path.dirname(dst)):
                fail('remote open("%s"): No such file or directory' % a[1])
            copy(src, dst)
    elif cmd == 'rm':
        p = real(rest[0])
        if not os.path.isfile(p):
            fail('Couldn\'t delete file: No such file or directory')
        os.unlink(p)
    elif cmd == 'rmdir':
        p = real(rest[0])
        try:
            os.rmdir(p)
        except OSError:
            fail('Couldn\'t remove directory: Failure')
    elif cmd == 'mkdir':
        p = real(rest[0])
        if os.path.exists(p):
            fail('Couldn\'t create directory: Failure')
        os.mkdir(p)
    elif cmd == 'rename':
        a, b = real(rest[0]), real(rest[1])
        if os.path.exists(b):
            fail('Couldn\'t rename file "%s" to "%s": Failure' % (rest[0], rest[1]))
        os.rename(a, b)
    else:
        fail('Invalid command.')
sys.exit(0)
