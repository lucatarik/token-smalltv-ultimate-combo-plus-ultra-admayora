"""The payload pushed to the clock: limits, computer readings, a dash (field left out) for what is unknown."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from token_tv import pushmon  # noqa: E402
from token_tv.sample import SYSTEM, snapshot  # noqa: E402

NOW = 1_800_000_000


class PushPayloadTests(unittest.TestCase):
    def row(self):
        return snapshot(NOW)['accounts']['claude_a']

    def test_limits_and_computer(self):
        body = pushmon.payload(self.row(), SYSTEM, NOW)
        self.assertEqual((body['s'], body['sr'], body['w'], body['wr'], body['ok']), (41, 120, 86, 4320, True))
        self.assertEqual((body['cpu'], body['ram'], body['dk'], body['dn'], body['up'], body['cs']),
                         (27, 58, 63, 1_300_000, 84_000, 'ok'))

    def test_unknown_values_are_left_out_not_zero(self):
        body = pushmon.payload(self.row(), dict(SYSTEM, cpu=None, net_down=None, claude_status=None), NOW)
        self.assertNotIn('cpu', body)
        self.assertNotIn('dn', body)
        self.assertEqual(body['cs'], '')

    def test_no_reading(self):
        row = dict(self.row(), status='auth_required', windows=[])
        self.assertEqual(pushmon.usage_fields(row, NOW), {'ok': False})


if __name__ == '__main__':
    unittest.main()
