"""Push Claude's limits, Claude's status and this computer's load to a SmallTV running smalltv-mod.

The firmware (github.com/giovi321/smalltv-mod, with the monitor layout of its Usage screen) draws what is
POSTed to /api/usage: {s, sr, w, wr, st, ok} for the limits, and {cpu, ram, dk, dn, up, cs} for the
computer. A reading that could not be taken is left out, so the clock shows a dash, never 0.
Run: python3 -m token_tv.pushmon [--device-url http://<ip>] [--config PATH]
"""
import argparse
import json
import time
import urllib.request

from token_tv import sysmon
from token_tv.sources import fetch_account, load_config
from token_tv.state import UsageStore

STATUS_WORDS = {'none': 'ok', 'minor': 'minor', 'major': 'major', 'critical': 'down', 'maintenance': 'maint'}
PUSH_SECONDS = 1


def usage_fields(row, now):
    """{s, sr, w, wr, st, ok} for a Claude account row; ok false when it has no reading."""
    windows = {w['label']: w for w in row.get('windows', [])}
    five, week = windows.get('5H'), windows.get('WEEK')
    if not (five or week) or row.get('status') not in ('ok', 'stale'):
        return {'ok': False}

    def minutes(window):
        reset = window.get('resets_at') if window else None
        return max(0, int((reset - now + 59) // 60)) if reset else 0
    fields = {'s': round(five['used_percent']) if five else 0, 'sr': minutes(five),
              'w': round(week['used_percent']) if week else 0, 'wr': minutes(week), 'ok': True}
    return fields


def payload(row, system, now):
    """The JSON the firmware takes; a value that is None is left out."""
    body = usage_fields(row, now)
    known = {'cpu': system.get('cpu'), 'ram': system.get('ram'), 'dk': system.get('disk'),
             'dn': system.get('net_down'), 'up': system.get('net_up')}
    body.update({key: int(value) for key, value in known.items() if value is not None})
    body['cs'] = STATUS_WORDS.get(system.get('claude_status'), '')
    return body


def push(url, body):
    request = urllib.request.Request(url.rstrip('/') + '/api/usage', json.dumps(body).encode(),
                                     {'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=3) as answer:
        return answer.status == 200


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--config', help='TokenTV config (default: the one `token-tv start` uses)')
    parser.add_argument('--device-url', help='the clock, e.g. http://192.168.1.50 (default: device_url of the config)')
    args = parser.parse_args()
    if args.config is None:
        from token_tv.cli import default_config
        args.config = default_config()
    config = load_config(args.config)
    url = args.device_url or config.get('device_url')
    if not url:
        raise SystemExit('No clock address: pass --device-url')
    if not url.startswith('http'):
        url = 'http://' + url
    accounts = [a for a in config.get('accounts', []) if a['provider'] == 'claude'][:1]
    if not accounts:
        raise SystemExit('No Claude account in the config: run `token-tv start --only claude` first')
    store = UsageStore(accounts, fetch=fetch_account, min_fetch_seconds={'claude': 300})
    machine = sysmon.Machine()
    machine.interval = PUSH_SECONDS
    machine.sample()
    failed = False
    while True:
        started = time.time()
        store.refresh()
        row = next(iter(store.snapshot()['accounts'].values()))
        system = {**machine.sample(), 'claude_status': sysmon.STATUS.get()}
        try:
            push(url, payload(row, system, started))
            failed = False
        except OSError as error:
            if not failed:  # say it once, not every second
                print(f'Clock at {url} not answering: {error}', flush=True)
            failed = True
        time.sleep(max(0, PUSH_SECONDS - (time.time() - started)))


if __name__ == '__main__':
    main()
