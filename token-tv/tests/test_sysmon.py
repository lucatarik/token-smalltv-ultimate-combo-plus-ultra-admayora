"""The Monitor face: Claude's limits, its status and the machine, with a dash for anything unknown."""
import sys
import time
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from token_tv import sysmon  # noqa: E402
from token_tv.sample import SYSTEM, snapshot  # noqa: E402


class MonitorTests(unittest.TestCase):
    def test_speed_text(self):
        self.assertEqual([sysmon.speed_text(v) for v in (None, 850, 12_000, 1_300_000, 25 << 20)],
                         ['--', '850B', '12K', '1.2M', '25M'])

    def test_weekly_reset_is_day_and_local_time(self):
        at = time.mktime((2026, 11, 6, 14, 5, 0, 0, 0, -1))  # a Friday
        self.assertEqual(sysmon.weekly_reset({'resets_at': at}), 'FRI 14:05')
        self.assertEqual(sysmon.weekly_reset(None), '--')

    def test_picture_is_240_square_and_equal_inputs_give_equal_pictures(self):
        now = 1_800_000_000
        first, second = sysmon.render_monitor(snapshot(now), now), sysmon.render_monitor(snapshot(now), now)
        self.assertEqual(first.size, (240, 240))
        self.assertEqual(first.tobytes(), second.tobytes())

    def test_unknown_machine_values_differ_from_zero(self):
        now = 1_800_000_000
        unknown = dict(SYSTEM, cpu=None, ram=None, net_down=None, claude_status=None)
        zero = dict(SYSTEM, cpu=0, ram=0, net_down=0, claude_status='none')
        for data, other in ((unknown, zero), (zero, unknown)):
            a = sysmon.render_monitor(dict(snapshot(now), system=data), now)
            b = sysmon.render_monitor(dict(snapshot(now), system=other), now)
            self.assertNotEqual(a.tobytes(), b.tobytes())

    def test_no_claude_account_still_renders(self):
        now = 1_800_000_000
        data = dict(snapshot(now, [('codex_a', 'CODEX A', 'codex', [('5H', 10, 3600)])]))
        self.assertEqual(sysmon.render_monitor(data, now).size, (240, 240))


if __name__ == '__main__':
    unittest.main()
