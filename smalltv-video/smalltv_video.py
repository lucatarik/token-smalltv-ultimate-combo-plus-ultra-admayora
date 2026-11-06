#!/usr/bin/env python3
"""Play a YouTube video, a video file, a gif or a square of your desktop on the SmallTV.

    smalltv_video.py <url|file|gif> [--device 192.168.0.29] [--fps 8] [--crop-x .5] [--crop-y .5]
    smalltv_video.py --desktop [--box X,Y,SIZE]
    smalltv_video.py --screen

YouTube is streamed through (nothing is downloaded) and starts again when the video ends. Video, gif and YouTube are cut to a square (centred unless --crop-x/--crop-y say otherwise), scaled to 240x240
and looped. --desktop opens a red square frame: drag it, change its size with the mouse wheel or the slider,
press Start. --screen sends the whole screen, cut to a square, with no window. Needs ffmpeg (x11grab for the desktop), yt-dlp for YouTube, tkinter for --desktop.

Wire format (firmware: features/usage/Screens.cpp): TCP port 8888, records 0xA5, y, n, then n rows of
240 little-endian RGB565 pixels; only rows that changed are sent.
"""
import argparse, json, os, shutil, socket, subprocess, sys, threading, time, urllib.request

W = H = 240
ROW = W * 2
FRAME = ROW * H
MIN_BOX = 64
RAW = ["-an", "-vf", None, "-f", "rawvideo", "-pix_fmt", "rgb565le", "-"]


def records(frame, prev):
    """Wire records that turn `prev` (or a blank screen) into `frame`; rows are merged when a gap is small."""
    out, y = [], 0
    while y < H:
        if prev is not None and frame[y * ROW:(y + 1) * ROW] == prev[y * ROW:(y + 1) * ROW]:
            y += 1
            continue
        end, quiet = y + 1, 0
        while end < H and end - y < 255 and quiet < 3:   # a run may bridge up to 2 unchanged rows
            same = prev is not None and frame[end * ROW:(end + 1) * ROW] == prev[end * ROW:(end + 1) * ROW]
            quiet = quiet + 1 if same else 0
            end += 1
        end -= quiet
        out.append(bytes((0xA5, y, end - y)) + frame[y * ROW:end * ROW])
        y = end
    return out


def raw_out(vf):
    return [vf if a is None else a for a in RAW]


def ffmpeg_cmd(src, fps, cx, cy, loop=True):
    """File/url (looped unless loop is False), cut to the largest square (anchored at cx, cy: .5 = centre), scaled."""
    crop = f"crop='min(iw,ih)':'min(iw,ih)':'(iw-min(iw,ih))*{cx}':'(ih-min(iw,ih))*{cy}'"
    return ["ffmpeg", "-loglevel", "error", "-re"] + (["-stream_loop", "-1"] if loop else []) + ["-i", src] + raw_out(f"{crop},scale={W}:{H}:flags=area,fps={fps}")


def clamp_box(x, y, size, sw, sh):
    size = max(MIN_BOX, min(int(size), sw, sh))
    return max(0, min(int(x), sw - size)), max(0, min(int(y), sh - size)), size


def grab_cmd(display, box, fps):
    """x11grab of the square box (x, y, size), scaled to the screen."""
    x, y, size = box
    if "." not in display.split(":")[-1]:
        display += ".0"
    return ["ffmpeg", "-loglevel", "error", "-f", "x11grab", "-framerate", str(fps), "-video_size", f"{size}x{size}",
            "-i", f"{display}+{x},{y}"] + raw_out(f"scale={W}:{H}:flags=area")


def screen_cmd(display, fps, cx, cy):
    """The whole screen (X11), cut to the largest square like a video, scaled to the clock."""
    if "." not in display.split(":")[-1]:
        display += ".0"
    crop = f"crop='min(iw,ih)':'min(iw,ih)':'(iw-min(iw,ih))*{cx}':'(ih-min(iw,ih))*{cy}'"
    return ["ffmpeg", "-loglevel", "error", "-f", "x11grab", "-framerate", str(fps), "-i", display] + raw_out(f"{crop},scale={W}:{H}:flags=area")


def is_youtube(src):
    return "youtube.com/" in src or "youtu.be/" in src


def stream_url(url):
    """The direct media address of a YouTube video (one file with sound and picture, at most 360 p); ffmpeg reads it."""
    out = subprocess.run(["yt-dlp", "--no-playlist", "-g", "-f", "b[height<=360]/w", url], check=True, capture_output=True, text=True)
    return out.stdout.split()[0]


def post(device, path, body):
    req = urllib.request.Request(f"http://{device}{path}", json.dumps(body).encode(), {"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=5).read()


class Streamer:
    """Arms the clock, connects, and sends whatever the current ffmpeg produces; play() swaps the source."""

    def __init__(self, device):
        self.device, self.proc, self.gen, self.latest = device, None, 0, None
        self.cv, self.stop = threading.Condition(), threading.Event()
        post(device, "/api/video", {"on": True})
        self.sock = None
        for _ in range(40):                      # the clock opens its port a moment after it is armed
            try:
                self.sock = socket.create_connection((device, 8888), timeout=5)
                break
            except OSError:
                time.sleep(.25)
        if not self.sock:
            raise OSError("the clock did not open port 8888")

    def play(self, cmd):
        with self.cv:
            self.gen += 1
            gen, old, self.latest = self.gen, self.proc, None
        if old:
            old.kill()
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
        threading.Thread(target=self._read, args=(gen, self.proc), daemon=True).start()

    def _read(self, gen, proc):                  # keeps only the newest frame: a slow clock drops frames, not time
        while True:
            data = proc.stdout.read(FRAME)
            if len(data) < FRAME:
                break
            with self.cv:
                if gen != self.gen:
                    return
                self.latest = data
                self.cv.notify()
        with self.cv:
            if gen == self.gen:
                self.latest = b""
                self.cv.notify()

    def run(self):
        """Send frames until the source ends, the clock hangs up or stop is set."""
        prev = None
        while not self.stop.is_set():
            with self.cv:
                if not self.cv.wait_for(lambda: self.latest is not None, .5):
                    continue
                frame, self.latest = self.latest, None
            if not frame:
                return
            for rec in records(frame, prev):
                self.sock.sendall(rec)
            prev = frame

    def close(self):
        self.stop.set()
        if self.proc:
            self.proc.kill()
        try:
            post(self.device, "/api/video", {"on": False})
        except OSError:
            pass


def desktop(streamer, fps, box, border=10):
    """A red square frame to drag (move) and wheel (resize), plus a slider and Start/Stop. Returns when closed."""
    import tkinter as tk
    root = tk.Tk()
    root.title("SmallTV capture")
    sw, sh = root.winfo_screenwidth(), root.winfo_screenheight()
    state = {"box": clamp_box(*(box or ((sw - sh // 2) // 2, sh // 4, sh // 2)), sw, sh), "on": False, "job": None}
    T = border                                   # frame thickness: it sits outside the captured square
    frame = []
    for _ in range(4):
        t = tk.Toplevel(root)
        t.overrideredirect(True)
        t.attributes("-topmost", True)
        t.configure(bg="red", cursor="fleur")           # the four-arrow cursor says: drag to move
        frame.append(t)
    size = tk.IntVar(value=state["box"][2])
    label = tk.Label(root)
    button = tk.Button(root, text="Start")

    def restart():
        state["job"] = None
        if state["on"]:
            streamer.play(grab_cmd(os.environ.get("DISPLAY", ":0"), state["box"], fps))

    def place(x, y, s):
        state["box"] = x, y, s = clamp_box(x, y, s, sw, sh)
        for t, g in zip(frame, (f"{s + 2 * T}x{T}+{x - T}+{y - T}", f"{s + 2 * T}x{T}+{x - T}+{y + s}",
                                f"{T}x{s}+{x - T}+{y}", f"{T}x{s}+{x + s}+{y}")):
            t.geometry(g)
        size.set(s)
        label.config(text=f"{s}x{s} at {x},{y}")
        if state["job"]:
            root.after_cancel(state["job"])
        state["job"] = root.after(300, restart)   # the stream follows once the frame has been still for a moment

    def grow(d):
        x, y, s = state["box"]
        place(x - d // 2, y - d // 2, s + d)

    grab = {}
    for t in frame:
        t.bind("<ButtonPress-1>", lambda e: grab.update(x=e.x_root, y=e.y_root, box=state["box"]))
        t.bind("<B1-Motion>", lambda e: place(grab["box"][0] + e.x_root - grab["x"], grab["box"][1] + e.y_root - grab["y"], grab["box"][2]))
        t.bind("<Button-4>", lambda e: grow(8))
        t.bind("<Button-5>", lambda e: grow(-8))

    def toggle():
        state["on"] = not state["on"]
        button.config(text="Stop" if state["on"] else "Start")
        if state["on"]:
            restart()
        else:
            streamer.proc and streamer.proc.kill()

    button.config(command=toggle)
    tk.Scale(root, from_=MIN_BOX, to=min(sw, sh), orient="horizontal", variable=size, length=260,
             command=lambda v: grow(int(v) - state["box"][2]) if int(v) != state["box"][2] else None).pack()
    label.pack()
    button.pack()
    root.protocol("WM_DELETE_WINDOW", root.quit)

    def watch():                                 # the clock hung up, or the stream died
        if streamer.stop.is_set():
            root.quit()
        root.after(300, watch)

    place(*state["box"])
    watch()
    threading.Thread(target=lambda: (streamer.run(), streamer.stop.set()), daemon=True).start()
    try:
        root.mainloop()
    finally:
        streamer.stop.set()
        root.destroy()


def main():
    a = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    a.add_argument("source", nargs="?", help="YouTube URL, video or gif file/URL")
    a.add_argument("--desktop", action="store_true", help="capture a square box of the desktop (X11)")
    a.add_argument("--screen", action="store_true", help="the whole screen (X11), cut to a square, no window")
    a.add_argument("--border", type=int, default=10, help="desktop frame thickness in pixels (default 10)")
    a.add_argument("--box", help="first desktop box: X,Y,SIZE")
    a.add_argument("--device", default=os.environ.get("SMALLTV_HOST", "192.168.0.29"))
    a.add_argument("--fps", type=int, default=8)
    a.add_argument("--crop-x", type=float, default=.5, help="0 left .. 1 right")
    a.add_argument("--crop-y", type=float, default=.5, help="0 top .. 1 bottom")
    args = a.parse_args()
    if bool(args.source) + args.desktop + args.screen != 1:
        a.error("give a source, or --desktop, or --screen")
    box = tuple(int(n) for n in args.box.split(",")) if args.box else None
    if box and len(box) != 3:
        a.error("--box is X,Y,SIZE")
    for tool in ["ffmpeg"] + (["yt-dlp"] if args.source and is_youtube(args.source) else []):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found")
    streamer = None
    try:
        streamer = Streamer(args.device)
        if args.desktop:
            desktop(streamer, args.fps, box, max(2, args.border))
        elif args.screen:
            streamer.play(screen_cmd(os.environ.get("DISPLAY", ":0"), args.fps, args.crop_x, args.crop_y))
            streamer.run()
        elif is_youtube(args.source):             # passthrough: no download; when the video ends it starts again
            while not streamer.stop.is_set():
                streamer.play(ffmpeg_cmd(stream_url(args.source), args.fps, args.crop_x, args.crop_y, loop=False))
                streamer.run()
        else:
            streamer.play(ffmpeg_cmd(args.source, args.fps, args.crop_x, args.crop_y))
            streamer.run()
    except KeyboardInterrupt:
        pass
    except OSError as e:
        print(f"stream ended: {e}", file=sys.stderr)
    finally:
        if streamer:
            streamer.close()


if __name__ == "__main__":
    main()
