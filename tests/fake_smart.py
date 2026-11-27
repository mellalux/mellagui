#!/usr/bin/env python3
"""A stand-in for `mellagui-smart` for the desktop card's tests ($MELLAGUI_SMART_BIN): --favourites prints the state kept in
$FAKE_SMART_STATE (JSON: [{id, name, online, kind, code, value}]), --set ID CODE VALUE changes it and is logged to $FAKE_SMART_LOG, and so are --device ID
and --scene ID --notify. $FAKE_SMART_SCENES ("id=Name;...") are the scenes with a button on the card."""
import json, os, sys

state_path = os.environ['FAKE_SMART_STATE']
log_path = os.environ.get('FAKE_SMART_LOG')


def load():
    with open(state_path) as f:
        return json.load(f)


a = sys.argv[1:]
if a[:1] == ['--favourites']:
    print('favs\t%d' % len(load()))
    for s in filter(None, os.environ.get('FAKE_SMART_SCENES', '').split(';')):
        print('scene\t%s\t%s' % tuple(s.split('=', 1)))
    for d in load():
        print('\t'.join(['fav', d['id'], d['name'], '1' if d.get('online', True) else '0', d['kind'], d['code'], str(d['value'])] +
                        ([d['extra']] if d.get('extra') else [])))
elif a[:1] == ['--scene'] and len(a) >= 2:
    if log_path:
        with open(log_path, 'a') as f:
            f.write(' '.join(a) + '\n')
elif a[:1] == ['--device'] and len(a) == 2:                     # the device's own view (a window in the real program): only logged here
    if log_path:
        with open(log_path, 'a') as f:
            f.write('device ' + a[1] + '\n')
elif a[:1] == ['--set'] and len(a) == 4:
    if log_path:
        with open(log_path, 'a') as f:
            f.write(' '.join(a[1:]) + '\n')
    st = load()
    for d in st:
        if d['id'] == a[1]:
            if d['kind'] == 'switch':
                d['value'] = 1 if a[3] == 'true' else 0
            elif d['kind'] == 'vacuum' and a[2] == d['code']:
                d['value'] = 1 if a[3] == 'true' else 0
            elif d['kind'] == 'blind':
                d['value'] = {'open': 100, 'close': 0}.get(a[3], d['value'])
    with open(state_path, 'w') as f:
        json.dump(st, f)
    print('sent')
else:
    sys.exit(2)
