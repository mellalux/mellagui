#!/usr/bin/env python3
"""A stand-in for `mellagui-smart` for the desktop card's tests ($MELLAGUI_SMART_BIN): --favourites prints the state kept in
$FAKE_SMART_STATE (JSON: [{id, name, online, kind, code, value}]), --set ID CODE VALUE changes it and is logged to $FAKE_SMART_LOG, and so are --device ID
and --scene ID --notify. $FAKE_SMART_SCENES ("id=Name;...") are the scenes with a button on the card; "id=Name|device/code,..." is one that toggles
those switches (--scene turns them the other way in the state)."""
import json, os, sys

state_path = os.environ['FAKE_SMART_STATE']
log_path = os.environ.get('FAKE_SMART_LOG')


def load():
    with open(state_path) as f:
        return json.load(f)


def scenes():
    for s in filter(None, os.environ.get('FAKE_SMART_SCENES', '').split(';')):
        sid, rest = s.split('=', 1)
        name, _, targets = rest.partition('|')
        yield sid, name, [t for t in targets.split(',') if t]


a = sys.argv[1:]
if a[:1] == ['--favourites']:
    print('favs\t%d' % len(load()))
    for sid, name, targets in scenes():
        extra = []
        if targets:
            on = any(d['value'] for d in load() if d['id'] + '/' + d['code'] in targets)
            extra = ['on' if on else 'off', ','.join(targets)]
        print('\t'.join(['scene', sid, name] + extra))
    for d in load():
        print('\t'.join(['fav', d['id'], d['name'], '1' if d.get('online', True) else '0', d['kind'], d['code'], str(d['value'])] +
                        ([d['extra']] if d.get('extra') else [])))
elif a[:1] == ['--scene'] and len(a) >= 2:
    if log_path:
        with open(log_path, 'a') as f:
            f.write(' '.join(a) + '\n')
    st = load()
    for sid, _, targets in scenes():
        if sid == a[1]:
            for d in st:
                if d['id'] + '/' + d['code'] in targets:
                    d['value'] = 0 if d['value'] else 1
    with open(state_path, 'w') as f:
        json.dump(st, f)
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
