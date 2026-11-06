import unittest
from smalltv_video import FRAME, H, ROW, clamp_box, ffmpeg_cmd, grab_cmd, is_youtube, screen_cmd, records


def frame(fill=0, rows=()):
    f = bytearray([fill]) * FRAME
    for y in rows:
        f[y * ROW:(y + 1) * ROW] = bytes([255]) * ROW
    return bytes(f)


class Records(unittest.TestCase):
    def rebuild(self, recs, base):
        out = bytearray(base)
        for r in recs:
            y, n = r[1], r[2]
            self.assertEqual(r[0], 0xA5)
            self.assertEqual(len(r), 3 + n * ROW)
            out[y * ROW:(y + n) * ROW] = r[3:]
        return bytes(out)

    def test_first_frame_goes_whole(self):
        f = frame(7)
        recs = records(f, None)
        self.assertEqual(self.rebuild(recs, bytes(FRAME)), f)
        self.assertEqual(sum(r[2] for r in recs), H)
        self.assertTrue(all(r[2] <= 255 for r in recs))

    def test_same_frame_sends_nothing(self):
        self.assertEqual(records(frame(3), frame(3)), [])

    def test_only_changed_rows_and_small_gaps_merge(self):
        prev = frame(0)
        new = frame(0, rows=(10, 12, 100))
        recs = records(new, prev)
        self.assertEqual([(r[1], r[2]) for r in recs], [(10, 3), (100, 1)])
        self.assertEqual(self.rebuild(recs, prev), new)

    def test_far_rows_stay_apart(self):
        recs = records(frame(0, rows=(5, 9)), frame(0))
        self.assertEqual([(r[1], r[2]) for r in recs], [(5, 1), (9, 1)])


class Misc(unittest.TestCase):
    def test_youtube(self):
        self.assertTrue(is_youtube("https://www.youtube.com/watch?v=x"))
        self.assertTrue(is_youtube("https://youtu.be/x"))
        self.assertFalse(is_youtube("clip.gif"))

    def test_youtube_passthrough_does_not_loop_the_input(self):
        self.assertNotIn("-stream_loop", ffmpeg_cmd("http://x/v.mp4", 8, .5, .5, loop=False))

    def test_ffmpeg_loops_and_outputs_rgb565(self):
        c = ffmpeg_cmd("a.gif", 8, .5, .5)
        self.assertIn("-stream_loop", c)
        self.assertEqual(c[c.index("-pix_fmt") + 1], "rgb565le")
        self.assertIn("scale=240:240", c[c.index("-vf") + 1])


class Desktop(unittest.TestCase):
    def test_clamp_box_keeps_it_square_and_on_screen(self):
        self.assertEqual(clamp_box(-5, 2000, 300, 1920, 1080), (0, 780, 300))
        self.assertEqual(clamp_box(100, 100, 5000, 1920, 1080), (100, 0, 1080))
        self.assertEqual(clamp_box(100, 100, 10, 1920, 1080), (100, 100, 64))

    def test_screen_cmd_crops_to_a_square(self):
        c = screen_cmd(":0", 8, .5, .5)
        self.assertEqual(c[c.index("-i") + 1], ":0.0")
        self.assertIn("min(iw,ih)", c[c.index("-vf") + 1])

    def test_grab_cmd(self):
        c = grab_cmd(":0", (10, 20, 300), 8)
        self.assertEqual(c[c.index("-i") + 1], ":0.0+10,20")
        self.assertEqual(c[c.index("-video_size") + 1], "300x300")
        self.assertIn("scale=240:240", c[c.index("-vf") + 1])
        self.assertEqual(grab_cmd(":1.2", (0, 0, 64), 5)[10], ":1.2+0,0")


if __name__ == "__main__":
    unittest.main()
