#!/usr/bin/env python3
"""A stand-in for nmcli in the tests (mellagui-status): the state is a JSON file, $FAKE_NM_STATE.

  {"radio": true, "dev": "wifi0", "ethernet": null | "Wired", "active": "Home" | null, "ip": "192.168.1.5",
   "networks": [{"ssid": "Home", "signal": 80, "security": "WPA2"}, ...], "known": ["Home"], "password": "secret"}

  FAKE_NM_LOG   every call is appended here as one line (the password is never an argument: it is read from the terminal)
It understands what mellagui-status uses: -t -f ... dev | dev wifi list | connection show | dev show DEV, radio wifi [on|off], dev wifi rescan,
connection up id X, dev disconnect DEV, dev wifi connect SSID and `--ask ... dev wifi connect SSID` (asks "Password: " on the terminal).
"""
import json, os, sys

path = os.environ['FAKE_NM_STATE']
log = os.environ.get('FAKE_NM_LOG')
args = sys.argv[1:]
if log:
    with open(log, 'a') as f:
        f.write(' '.join(args) + '\n')
st = json.load(open(path))


def save():
    json.dump(st, open(path, 'w'))


def esc(s):
    return s.replace('\\', '\\\\').replace(':', '\\:')


ask = '--ask' in args
args = [a for a in args if a not in ('--ask', '-t')]
while '--wait' in args:
    i = args.index('--wait')
    del args[i:i + 2]
if '-f' in args:
    i = args.index('-f')
    fields = args[i + 1].split(',')
    del args[i:i + 2]
else:
    fields = []
if '--rescan' in args:
    i = args.index('--rescan')
    del args[i:i + 2]

if args[:1] == ['dev'] and len(args) == 1:
    if st.get('dev'):
        print('%s:wifi:%s:%s' % (st['dev'], 'connected' if st['active'] else 'disconnected', esc(st['active'] or '')))
    if st.get('ethernet'):
        print('enp1s0:ethernet:connected:%s' % esc(st['ethernet']))
    print('lo:loopback:connected (externally):lo')
elif args[:2] == ['radio', 'wifi']:
    if len(args) == 3:
        st['radio'] = args[2] == 'on'
        if not st['radio']:
            st['active'] = None
        save()
    else:
        print('enabled' if st['radio'] else 'disabled')
elif args[:3] == ['dev', 'wifi', 'list']:
    if not st['radio']:
        sys.exit(0)
    for n in st['networks']:
        print('%s:%s:%d:%s' % ('*' if n['ssid'] == st['active'] else ' ', esc(n['ssid']), n['signal'], n.get('security') or '--'))
elif args[:2] == ['connection', 'show']:
    for k in st.get('known', []):
        print('%s:802-11-wireless' % esc(k))
elif args[:2] == ['dev', 'show']:
    print('IP4.ADDRESS[1]:%s/24' % st['ip'])
elif args[:3] == ['dev', 'wifi', 'rescan']:
    pass
elif args[:3] == ['connection', 'up', 'id']:
    if args[3] not in st.get('known', []):
        print('Error: unknown connection')
        sys.exit(10)
    st['active'] = args[3]
    save()
    print('Connection successfully activated')
elif args[:2] == ['dev', 'disconnect']:
    st['active'] = None
    save()
    print('Device successfully disconnected.')
elif args[:3] == ['dev', 'wifi', 'connect']:
    ssid = args[3]
    net = [n for n in st['networks'] if n['ssid'] == ssid]
    if not net:
        print('Error: No network with SSID \'%s\' found.' % ssid)
        sys.exit(10)
    if net[0].get('security'):
        if not ask:
            print('Error: Password required')
            sys.exit(4)
        sys.stdout.write('Password: ')
        sys.stdout.flush()
        pw = sys.stdin.readline().rstrip('\r\n')
        if pw != st.get('password'):
            print('\nError: Connection activation failed: (7) Secrets were required, but not provided.')
            sys.exit(4)
        st['known'] = st.get('known', []) + [ssid]
    st['active'] = ssid
    save()
    print('\nDevice \'%s\' successfully activated.' % st['dev'])
else:
    print('Error: unsupported by the fake: %s' % ' '.join(args))
    sys.exit(2)
