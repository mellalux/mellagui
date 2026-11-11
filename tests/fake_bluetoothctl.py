#!/usr/bin/env python3
"""A stand-in for bluetoothctl in the tests (mellagui-status): the state is a JSON file, $FAKE_BT_STATE.

  {"controller": true, "powered": true,
   "devices": [{"mac": "AA:BB:CC:DD:EE:01", "name": "Headset", "paired": true, "connected": false}],
   "nearby": [{"mac": "AA:BB:CC:DD:EE:09", "name": "New Speaker"}],     # shown by a scan (`--timeout N scan on`) as unpaired devices
   "pair_fails": false}

  FAKE_BT_LOG   every call is appended here as one line
It prints what bluetoothctl 5.6x prints for: show, power on|off, devices [Paired], paired-devices, info MAC, connect, disconnect, pair, trust,
--timeout N scan on.
"""
import json, os, sys

path = os.environ['FAKE_BT_STATE']
log = os.environ.get('FAKE_BT_LOG')
args = sys.argv[1:]
if log:
    with open(log, 'a') as f:
        f.write(' '.join(args) + '\n')
st = json.load(open(path))


def save():
    json.dump(st, open(path, 'w'))


def find(mac):
    for d in st['devices']:
        if d['mac'] == mac:
            return d
    return None


if args[:1] == ['--timeout']:
    args = args[2:]
if not st.get('controller', True):
    print('No default controller available')
    sys.exit(1)
cmd = args[0] if args else ''
if cmd == 'show':
    print('Controller 00:11:22:33:44:55 (public)\n\tName: testbox\n\tAlias: testbox\n\tClass: 0x0000010c\n\tPowered: %s\n\tDiscoverable: no\n\tPairable: yes\n\tDiscovering: no'
          % ('yes' if st['powered'] else 'no'))
elif cmd == 'power':
    st['powered'] = args[1] == 'on'
    if not st['powered']:
        for d in st['devices']:
            d['connected'] = False
    save()
    print('Changing power %s succeeded' % args[1])
elif cmd in ('devices', 'paired-devices'):
    only_paired = cmd == 'paired-devices' or args[1:2] == ['Paired']
    for d in st['devices']:
        if not only_paired or d['paired']:
            print('Device %s %s' % (d['mac'], d['name']))
elif cmd == 'info':
    d = find(args[1])
    if not d:
        print('Device %s not available' % args[1])
        sys.exit(1)
    print('Device %s (public)\n\tName: %s\n\tAlias: %s\n\tPaired: %s\n\tTrusted: yes\n\tBlocked: no\n\tConnected: %s'
          % (d['mac'], d['name'], d['name'], 'yes' if d['paired'] else 'no', 'yes' if d['connected'] else 'no'))
elif cmd == 'connect':
    d = find(args[1])
    if d and d['paired'] and st['powered']:
        d['connected'] = True
        save()
        print('Attempting to connect to %s\nConnection successful' % args[1])
    else:
        print('Attempting to connect to %s\nFailed to connect: org.bluez.Error.Failed' % args[1])
        sys.exit(1)
elif cmd == 'disconnect':
    d = find(args[1])
    if d:
        d['connected'] = False
        save()
    print('Attempting to disconnect from %s\nSuccessful disconnected' % args[1])
elif cmd == 'pair':
    d = find(args[1])
    if d and not st.get('pair_fails'):
        d['paired'] = True
        save()
        print('Attempting to pair with %s\nPairing successful' % args[1])
    else:
        print('Attempting to pair with %s\nFailed to pair: org.bluez.Error.AuthenticationFailed' % args[1])
        sys.exit(1)
elif cmd == 'trust':
    print('Changing %s trust succeeded' % args[1])
elif cmd == 'scan':
    for n in st.get('nearby', []):
        if not find(n['mac']):
            st['devices'].append({'mac': n['mac'], 'name': n['name'], 'paired': False, 'connected': False})
    save()
    print('Discovery started')
else:
    print('Invalid command')
    sys.exit(1)
