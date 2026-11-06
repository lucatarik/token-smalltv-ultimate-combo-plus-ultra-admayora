"""Monitor: a still Neon picture of Claude's session and weekly limits, Claude's service status and the machine.

A snapshot may carry a 'system' reading (ram, disk, net_down, net_up, claude_status); the demo and
the tests do, so they never touch the machine or the network. Without one the face samples this computer
(standard library only; /proc and /sys on Linux, a missing value is a dash on other systems) and reads
Claude's public status page in a background thread, so a render never waits for the network.
An unknown value is None and is drawn as a dash, never as 0.
"""
import json
import shutil
import threading
import time
import urllib.request
from pathlib import Path

from token_tv.display import account_label, reading, time_left
from token_tv.themes import ACCENT, SIZE, Canvas, band, face, gauge, mix, number_text, place_bot, bot_sprite, rgb

STATUS_URL = 'https://status.claude.com/api/v2/status.json'
STATUS_SECONDS = 300  # Claude's status is looked up at most this often
SAMPLE_SECONDS = 3  # the machine is read at most this often; live.py decides how rarely the clock gets a new picture
LEVELS = {'none': ('OK', '#2cf5b0'), 'minor': ('MINOR', '#ffc23d'), 'major': ('MAJOR', '#ff3d81'),
          'critical': ('DOWN', '#ff3d81'), 'maintenance': ('MAINT', '#38d6ff')}
DIM = '#8f86b8'
DAYS = ('MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT', 'SUN')


# --- the machine -----------------------------------------------------------------------------

def read_text(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def cpu_times():
    """(busy, total) jiffies of all CPUs since boot, or None."""
    text = read_text('/proc/stat')
    if not text or not text.startswith('cpu '):
        return None
    numbers = [int(n) for n in text.splitlines()[0].split()[1:]]
    idle = numbers[3] + (numbers[4] if len(numbers) > 4 else 0)  # idle and iowait
    return sum(numbers[:8]) - idle, sum(numbers[:8])


def memory():
    """Percent of RAM in use (what is not available to programs), or None."""
    text = read_text('/proc/meminfo')
    if not text:
        return None
    fields = {line.split(':')[0]: int(line.split(':')[1].split()[0]) for line in text.splitlines() if ':' in line}
    total, free = fields.get('MemTotal'), fields.get('MemAvailable')
    return round(100 * (total - free) / total) if total and free is not None else None


def disk():
    """Percent of the disk holding the home folder that is in use, or None."""
    try:
        usage = shutil.disk_usage(Path.home())
    except OSError:
        return None
    return round(100 * usage.used / usage.total) if usage.total else None


def net_bytes():
    """(received, sent) bytes of all network interfaces but loopback, or None."""
    text = read_text('/proc/net/dev')
    if not text:
        return None
    down = up = 0
    for line in text.splitlines()[2:]:
        name, _, rest = line.partition(':')
        if name.strip() != 'lo':
            numbers = rest.split()
            down, up = down + int(numbers[0]), up + int(numbers[8])
    return down, up


class Machine:
    """Samples the computer; the network speed is the change in the byte counters since the last sample."""

    def __init__(self):
        self.lock = threading.Lock()
        self.last = None  # (monotonic, (down, up) counters)
        self.cpu = None  # the CPU counters at the last sample
        self.value = None  # the reading and when it was taken
        self.interval = SAMPLE_SECONDS
        self.taken = 0

    def sample(self, now=None):
        with self.lock:
            clock = time.monotonic() if now is None else now
            if self.value and clock - self.taken < self.interval:
                return self.value
            counters = net_bytes()
            down = up = None
            if counters and self.last and self.last[1] and clock > self.last[0]:
                span = clock - self.last[0]
                down, up = (max(0, a - b) / span for a, b in zip(counters, self.last[1]))
            now_cpu, usage = cpu_times(), None
            if now_cpu and self.cpu and now_cpu[1] > self.cpu[1]:
                usage = round(100 * (now_cpu[0] - self.cpu[0]) / (now_cpu[1] - self.cpu[1]))
            self.cpu = now_cpu
            self.value = {'cpu': usage, 'ram': memory(), 'disk': disk(), 'net_down': down, 'net_up': up}
            self.last, self.taken = (clock, counters), clock
            return self.value


MACHINE = Machine()
MACHINE.sample()  # the counters' starting point: the first picture has no speed yet, only a dash


# --- Claude's status ---------------------------------------------------------------------------

class Status:
    """Claude's service status from its public status page, refreshed in the background."""

    def __init__(self):
        self.value = None  # the indicator ('none', 'minor', ...) or None while unknown
        self.asked = 0
        self.lock = threading.Lock()

    def fetch(self):
        try:
            with urllib.request.urlopen(urllib.request.Request(STATUS_URL, headers={'User-Agent': 'token-tv'}), timeout=5) as page:
                indicator = json.load(page)['status']['indicator']
            value = indicator if indicator in LEVELS else None
        except (OSError, ValueError, KeyError, TypeError):
            value = None
        with self.lock:
            self.value = value

    def get(self):
        with self.lock:
            due = time.monotonic() - self.asked >= STATUS_SECONDS
            if due:
                self.asked = time.monotonic()
            value = self.value
        if due:
            threading.Thread(target=self.fetch, daemon=True).start()
        return value


STATUS = Status()


def system_of(snapshot):
    return snapshot.get('system') or {**MACHINE.sample(), 'claude_status': STATUS.get()}


# --- the picture -----------------------------------------------------------------------------

def speed_text(rate):
    """Bytes a second as at most four characters and a unit: 850B, 12K, 1.2M."""
    if rate is None:
        return '--'
    for unit, size in (('M', 1 << 20), ('K', 1 << 10)):
        if rate >= size:
            value = rate / size
            return f'{value:.1f}{unit}' if value < 10 else f'{round(value)}{unit}'
    return f'{round(rate)}B'


def weekly_reset(value):
    """The weekly window's reset as day and local time, e.g. THU 14:00, or a dash."""
    at = value.get('resets_at') if value else None
    if not at:
        return '--'
    local = time.localtime(at)
    return f'{DAYS[local.tm_wday]} {local.tm_hour:02d}:{local.tm_min:02d}'


def claude_row(snapshot):
    """The Claude account to show: the working one, else the first; a placeholder without any."""
    rows = [dict(row, key=key) for key, row in snapshot.get('accounts', {}).items() if row['provider'] == 'claude']
    return next((row for row in rows if row.get('active')), rows[0] if rows else
                {'key': 'claude', 'provider': 'claude', 'alias': 'CLAUDE', 'status': 'auth_required', 'windows': []})


def panel(cv, box, colour):
    cv.back.rounded_rectangle(box, radius=10, fill=mix('#06050e', colour, .05))
    cv.glow.rounded_rectangle(box, radius=10, outline=rgb(colour) + (255,), width=3)
    cv.draw.rounded_rectangle(box, radius=10, outline=mix(colour, '#ffffff', .45), width=1)


def tube(cv, box, used, colour, old=False):
    gauge(cv, box, used, band('neon', used or 0), gap=3, shape='round', radius=3, track='#15122a', empty='#4a4170',
          stale=old, glow=True)


def number(cv, xy, used, colour, size, old=False):
    """The used % in Oxanium with its glow, a dash when unknown; returns the right edge."""
    font = face('oxanium.woff2', size, 800)
    text = number_text(used)
    cv.text(xy, text, font, '#ff5d8f' if old else '#ffffff')
    cv.glow.text(xy, text, font=font, fill=rgb(colour) + (255,), stroke_width=2, stroke_fill=rgb(colour) + (255,))
    right = xy[0] + cv.draw.textlength(text, font=font) + 2
    if used is not None:
        cv.text((right, xy[1] + size * .45), '%', face('oxanium.woff2', round(size * .5), 700), mix(colour, '#ffffff', .6))
        right += size * .5
    return right


def limit_panel(cv, y, name, value, row, right_text, colour):
    used, _, old, _ = reading(row, value) if value else (None, '', row['status'] != 'ok', '')
    panel(cv, (5, y, 234, y + 58), colour)
    cv.text((16, y + 6), name + (' OLD' if old else ''), face('orbitron.ttf', 9, 700), '#ff5d8f' if old else DIM)
    number(cv, (16, y + 14), used, colour, 28, old)
    cv.text((226, y + 18), right_text, face('oxanium.woff2', 17, 700), '#ffffff', glow=rgb(colour) + (170,), anchor='ra')
    tube(cv, (14, y + 46, 226, y + 52), used, colour, old)


def machine_panel(cv, y, system, colour):
    panel(cv, (5, y, 234, y + 71), colour)
    for x, name, value in ((14, 'CPU', system.get('cpu')), (88, 'RAM', system.get('ram')), (162, 'DISK', system.get('disk'))):
        cv.text((x, y + 6), name, face('orbitron.ttf', 9, 700), DIM)
        number(cv, (x, y + 17), value, colour, 22)
        tube(cv, (x, y + 45, x + 62, y + 50), value, colour)
    net = face('oxanium.woff2', 14, 700)
    cv.text((16, y + 53), 'DN ' + speed_text(system.get('net_down')), net, '#ffffff', glow=rgb(colour) + (150,))
    cv.text((226, y + 52), 'UP ' + speed_text(system.get('net_up')), net, '#ffffff', glow=rgb(colour) + (150,), anchor='ra')


def render_monitor(snapshot, now=None):
    """The still Neon picture; the numbers come from the snapshot and from this computer."""
    row, system = claude_row(snapshot), system_of(snapshot)
    colour, _ = ACCENT['neon']['claude']
    cv = Canvas('#04030a')
    for y in range(4, SIZE, 8):
        for x in range(4, SIZE, 8):
            cv.back.point((x, y), fill='#0e0b1c')
    place_bot(cv.ink, bot_sprite('claude', 26, 26, mix(colour, '#ffffff', .35)), (10, 6, 26, 26))
    place_bot(cv.bloom, bot_sprite('claude', 26, 26, colour), (10, 6, 26, 26))
    title = face('orbitron.ttf', 13, 700)
    cv.text((44, 11), account_label(row), title, '#ffffff')
    clock = time.strftime('%H:%M', time.localtime(time.time() if now is None else now))
    cv.text((44 + cv.draw.textlength(account_label(row), font=title) + 12, 10), clock, face('oxanium.woff2', 15, 700),
            mix(colour, '#ffffff', .4), glow=rgb(colour) + (170,))
    label, ink = LEVELS.get(system.get('claude_status'), ('?', DIM))
    cv.text((228, 11), label, face('orbitron.ttf', 12, 700), ink, glow=rgb(ink) + (190,), anchor='ra')
    windows = {w['label']: w for w in row.get('windows', [])}
    five, week = windows.get('5H'), windows.get('WEEK')
    limit_panel(cv, 36, 'SESSION', five, row, time_left(five.get('resets_at')) if five else '--', colour)
    limit_panel(cv, 98, 'WEEKLY', week, row, weekly_reset(week), colour)
    machine_panel(cv, 162, system, colour)
    return cv.finish(blur=3)
