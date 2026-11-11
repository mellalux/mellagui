#!/usr/bin/env python3
"""A stand-in for bluetoothctl in the tests (mellagui-status): the state is a JSON file, $FAKE_BT_STATE.

  {"controller": true, "powered": true,
   "devices": [{"mac": "AA:BB:CC:DD:EE:01", "name": "Headset", "paired": true, "connected": false}],
   "nearby": [{"mac": "AA:BB:CC:DD:EE:09", "name": "New Speaker"}],     # shown by a scan (`--timeout N scan on`) as unpaired devices
   "pair_fails": false, "connect_error": "br-connection-profile-unavailable"}; a device may have "battery": 0..100 (shown by `info` while connected,
   announced with a [CHG] line on connect)      # connect_error: every connect fails with this reason

  FAKE_BT_LOG   every call is appended here as one line
It prints what bluetoothctl 5.6x / 5.72 prints for: show, power on|off, devices [Paired], paired-devices, info MAC, connect, disconnect, pair, trust,
--timeout N scan on - and, started without arguments, an interactive session (what mellagui-bluetooth runs on a pty): prompts "[bluetooth]# ",
[NEW] / [CHG] / [DEL] events while scanning (each "nearby" device may carry "rssi" and "pair": "just_works" | "confirm" | "pin" | "display"), and the
agent's questions for `pair MAC`: "[agent] Confirm passkey 123456 (yes/no): ", "[agent] Enter PIN code: " (the PIN is 0000), "[agent] Passkey: 482913".
"""
import json, os, sys

path = os.environ['FAKE_BT_STATE']
log = os.environ.get('FAKE_BT_LOG')
args = sys.argv[1:]
st = json.load(open(path))
if args and log:
    with open(log, 'a') as f:
        f.write(' '.join(args) + '\n')


def save():
    json.dump(st, open(path, 'w'))


def find(mac):
    for d in st['devices']:
        if d['mac'] == mac:
            return d
    return None


PROMPT = '[bluetooth]# '


def interactive():
    import time
    def out(text, nl=True):
        sys.stdout.write(text + ('\n' if nl else ''))
        sys.stdout.flush()
    out('Waiting to connect to bluetoothd...' + PROMPT + 'Agent registered')
    for line in sys.stdin:
        line = line.strip()
        if log:
            with open(log, 'a') as f:
                f.write('session: ' + line + '\n')
        w = line.split()
        c = w[0] if w else ''
        if c == 'quit':
            break
        elif c == 'scan':
            if w[1] == 'on':
                out(PROMPT + 'Discovery started')
                out(PROMPT + '[CHG] Controller 88:53:95:2B:EE:5A Discovering: yes')
                for n in st.get('nearby', []):
                    out(PROMPT + '[NEW] Device %s %s' % (n['mac'], n['name']))
                    if n.get('rssi'):
                        out(PROMPT + '[CHG] Device %s RSSI: 0x%08x (%d)' % (n['mac'], n['rssi'] & 0xffffffff, n['rssi']))
                out(PROMPT + '[NEW] Device 6E:8C:96:6C:5B:E3 6E-8C-96-6C-5B-E3')
            else:
                out(PROMPT + 'Discovery stopped')
                out(PROMPT + '[CHG] Controller 88:53:95:2B:EE:5A Discovering: no')
        elif c == 'power':
            st['powered'] = w[1] == 'on'
            save()
            out(PROMPT + 'Changing power %s succeeded' % w[1])
            out(PROMPT + '[CHG] Controller 88:53:95:2B:EE:5A Powered: %s' % ('yes' if st['powered'] else 'no'))
        elif c == 'connect':
            d = find(w[1])
            if d and d['paired'] and st.get('connect_error'):
                out(PROMPT + 'Attempting to connect to %s' % w[1])
                out(PROMPT + 'Failed to connect: org.bluez.Error.Failed %s' % st['connect_error'])
            elif d and d['paired']:
                d['connected'] = True
                save()
                out(PROMPT + 'Attempting to connect to %s' % w[1])
                out(PROMPT + '[CHG] Device %s Connected: yes' % w[1])
                if d.get('battery') is not None:
                    out(PROMPT + '[CHG] Device %s Battery Percentage: 0x%02x (%d)' % (w[1], d['battery'], d['battery']))
                out(PROMPT + 'Connection successful')
            else:
                out(PROMPT + 'Failed to connect: org.bluez.Error.Failed')
        elif c == 'disconnect':
            d = find(w[1])
            if d:
                d['connected'] = False
                save()
            out(PROMPT + 'Attempting to disconnect from %s' % w[1])
            out(PROMPT + '[CHG] Device %s Connected: no' % w[1])
            out(PROMPT + 'Successful disconnected')
        elif c == 'remove':
            st['devices'] = [d for d in st['devices'] if d['mac'] != w[1]]
            save()
            out(PROMPT + '[DEL] Device %s' % w[1])
            out(PROMPT + 'Device has been removed')
        elif c == 'trust':
            out(PROMPT + 'Changing %s trust succeeded' % w[1])
        elif c == 'pair':
            n = [x for x in st.get('nearby', []) if x['mac'] == w[1]]
            mode = n[0].get('pair', 'just_works') if n else 'just_works'
            out(PROMPT + 'Attempting to pair with %s' % w[1])
            ok = True
            if mode == 'confirm':
                out('[agent] Confirm passkey 123456 (yes/no): ', nl=False)
                ok = sys.stdin.readline().strip() == 'yes'
                out('')
            elif mode == 'pin':
                out('[agent] Enter PIN code: ', nl=False)
                ok = sys.stdin.readline().strip() == '0000'
                out('')
            elif mode == 'display':
                out(PROMPT + '[agent] Passkey: 482913')
                time.sleep(1.0)
            if ok and not st.get('pair_fails'):
                if n and not find(w[1]):
                    st['devices'].append({'mac': w[1], 'name': n[0]['name'], 'paired': True, 'connected': False})
                elif find(w[1]):
                    find(w[1])['paired'] = True
                save()
                out(PROMPT + '[CHG] Device %s Paired: yes' % w[1])
                out(PROMPT + 'Pairing successful')
            else:
                out(PROMPT + 'Failed to pair: org.bluez.Error.AuthenticationFailed')
        elif c == 'cancel-pairing':
            out(PROMPT + 'Cancel pairing successful')
        else:
            out(PROMPT + 'Invalid command')
    sys.exit(0)


if not args:
    interactive()
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
    bat = ('\n\tBattery Percentage: 0x%02x (%d)' % (d['battery'], d['battery'])) if d.get('battery') is not None and d['connected'] else ''
    print('Device %s (public)\n\tName: %s\n\tAlias: %s\n\tPaired: %s\n\tTrusted: yes\n\tBlocked: no\n\tConnected: %s%s'
          % (d['mac'], d['name'], d['name'], 'yes' if d['paired'] else 'no', 'yes' if d['connected'] else 'no', bat))
elif cmd == 'connect':
    d = find(args[1])
    if d and d['paired'] and st.get('connect_error'):
        print('Attempting to connect to %s\nFailed to connect: org.bluez.Error.Failed %s' % (args[1], st['connect_error']))
        sys.exit(1)
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
