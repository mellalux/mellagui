#!/usr/bin/env python3
"""A stand-in for systemctl and journalctl in the tests (mellagui-top): one script, told apart by the first word.

  list-units ...                     prints three services (the third is failed)
  [--user] [--no-ask-password] start|stop|restart|enable|disable UNIT     logged, exit 0 (or 1 with "Interactive authentication required." when
                                      FAKE_SYSTEMCTL_DENY is set)
  journalctl -u UNIT ...             prints a few lines (with the unit's name)
Every call is appended to $FAKE_SYSTEMCTL_LOG as one line of its arguments.
"""
import os, sys

args = sys.argv[1:]
log = os.environ.get('FAKE_SYSTEMCTL_LOG')
tool = os.path.basename(sys.argv[0])
if log:
    with open(log, 'a') as f:
        f.write('%s %s\n' % ('journalctl' if 'journal' in tool else 'systemctl', ' '.join(args)))

if 'journal' in tool or os.environ.get('FAKE_AS_JOURNAL'):
    unit = args[args.index('-u') + 1] if '-u' in args else '?'
    for i in range(1, 8):
        print('Nov 10 12:00:%02d fakehost %s[%d]: line %d of the journal' % (i, unit, 100 + i, i))
    print('Nov 10 12:00:09 fakehost %s[107]: failed to do something' % unit)
    sys.exit(0)

verbs = [a for a in args if not a.startswith('--')]
scope = 'user' if '--user' in args else 'system'
if verbs and verbs[0] == 'list-units':
    print('alpha-%s.service        loaded active   running Alpha daemon' % scope)
    print('beta-%s.service         loaded inactive dead    Beta job' % scope)
    print('gamma-%s.service        loaded failed   failed  Gamma that broke' % scope)
    sys.exit(0)
if verbs and verbs[0] in ('start', 'stop', 'restart', 'enable', 'disable'):
    if os.environ.get('FAKE_SYSTEMCTL_DENY'):
        print('Failed to %s %s: Interactive authentication required.' % (verbs[0], verbs[1]))
        sys.exit(1)
    sys.exit(0)
print('unknown command', args)
sys.exit(2)
