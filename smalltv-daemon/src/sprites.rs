//! Sprite proxy for the clock's theme walkers. The clock cannot do TLS or decode PNG/GIF, so it asks this process
//! (http://<pc>:8899/sprite?s=WxH&f=F&a=<tilt>&m=<0|1>&u=<image address>) and gets back a ready blob: the sprite scaled
//! to fit, F animation frames, mirrored when m is 1 and then turned clockwise by `a` degrees, as RGB565. Only the clock's
//! own address may ask, and only for allowed image addresses.
//!
//! Blob "STR2": "STR2", u16 width, u16 height, u8 frames, u8 1, u16 zero; 16 RGB565 colours, little-endian (index 0
//! is transparent, so at most 15 real colours: the sprite is reduced to them); then for every frame the rows of 4-bit
//! pixels, two to a byte (the left one in the high half), every row padded to a whole byte.
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{IpAddr, TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::time::Duration;

pub const PORT: u16 = 8899;
const DEFAULT_ALLOW: &str = "https://raw.githubusercontent.com/PokeAPI/sprites/";
/// How full of the box the sprite may be: it must still fit after being turned by 45 degrees (a square of side a needs 1.42 a).
const FILL: f64 = 0.72;

#[derive(Clone)]
pub struct Image {
    pub w: usize,
    pub h: usize,
    pub px: Vec<[u8; 4]>,
}

impl Image {
    fn blank(w: usize, h: usize) -> Image {
        Image { w, h, px: vec![[0; 4]; w * h] }
    }
    fn get(&self, x: i64, y: i64) -> [u8; 4] {
        if x < 0 || y < 0 || x >= self.w as i64 || y >= self.h as i64 { [0; 4] } else { self.px[y as usize * self.w + x as usize] }
    }
}

/// Every frame of a PNG (one) or GIF (all, composed onto the full canvas).
pub fn decode(bytes: &[u8]) -> Result<Vec<Image>, String> {
    if bytes.starts_with(b"GIF8") {
        let mut options = gif::DecodeOptions::new();
        options.set_color_output(gif::ColorOutput::RGBA);
        let mut decoder = options.read_info(bytes).map_err(|e| e.to_string())?;
        let (cw, ch) = (decoder.width() as usize, decoder.height() as usize);
        let mut canvas = Image::blank(cw, ch);
        let mut frames = Vec::new();
        while let Some(frame) = decoder.read_next_frame().map_err(|e| e.to_string())? {
            let before = canvas.clone();
            for y in 0..frame.height as usize {
                for x in 0..frame.width as usize {
                    let p = &frame.buffer[(y * frame.width as usize + x) * 4..][..4];
                    let (cx, cy) = (frame.left as usize + x, frame.top as usize + y);
                    if p[3] > 0 && cx < cw && cy < ch {
                        canvas.px[cy * cw + cx] = [p[0], p[1], p[2], p[3]];
                    }
                }
            }
            frames.push(canvas.clone());
            match frame.dispose {
                gif::DisposalMethod::Background => {
                    for y in frame.top as usize..(frame.top as usize + frame.height as usize).min(ch) {
                        for x in frame.left as usize..(frame.left as usize + frame.width as usize).min(cw) {
                            canvas.px[y * cw + x] = [0; 4];
                        }
                    }
                }
                gif::DisposalMethod::Previous => canvas = before,
                _ => {}
            }
        }
        if frames.is_empty() { Err("empty gif".into()) } else { Ok(frames) }
    } else {
        let mut decoder = png::Decoder::new(bytes);
        decoder.set_transformations(png::Transformations::EXPAND | png::Transformations::STRIP_16);
        let mut reader = decoder.read_info().map_err(|e| e.to_string())?;
        let mut buf = vec![0; reader.output_buffer_size()];
        let info = reader.next_frame(&mut buf).map_err(|e| e.to_string())?;
        let (w, h) = (info.width as usize, info.height as usize);
        let channels = info.color_type.samples();
        let mut img = Image::blank(w, h);
        for i in 0..w * h {
            let p = &buf[i * channels..][..channels];
            img.px[i] = match info.color_type {
                png::ColorType::Rgba => [p[0], p[1], p[2], p[3]],
                png::ColorType::Rgb => [p[0], p[1], p[2], 255],
                png::ColorType::GrayscaleAlpha => [p[0], p[0], p[0], p[1]],
                _ => [p[0], p[0], p[0], 255],
            };
        }
        Ok(vec![img])
    }
}

fn rgb565(c: [u8; 3]) -> u16 {
    ((c[0] as u16 >> 3) << 11) | ((c[1] as u16 >> 2) << 5) | (c[2] as u16 >> 3)
}

/// At most `max` colours that stand for all of `counts` (colour, how many pixels): median cut.
fn palette(counts: &HashMap<[u8; 3], u32>, max: usize) -> Vec<[u8; 3]> {
    let mut boxes: Vec<Vec<([u8; 3], u32)>> = vec![counts.iter().map(|(c, n)| (*c, *n)).collect()];
    while boxes.len() < max {
        // split the box that spans the widest range of one channel
        let range = |b: &Vec<([u8; 3], u32)>| (0..3).map(|ch| {
            let (lo, hi) = b.iter().fold((255u8, 0u8), |(lo, hi), (c, _)| (lo.min(c[ch]), hi.max(c[ch])));
            (hi - lo, ch)
        }).max().unwrap();
        let Some((i, (spread, ch))) = boxes.iter().enumerate().filter(|(_, b)| b.len() > 1).map(|(i, b)| (i, range(b))).max_by_key(|(_, (r, _))| *r) else { break };
        if spread == 0 {
            break;
        }
        let mut b = boxes.swap_remove(i);
        b.sort_by_key(|(c, _)| c[ch]);
        let total: u64 = b.iter().map(|(_, n)| *n as u64).sum();
        let (mut acc, mut cut) = (0u64, 1);
        for (k, (_, n)) in b.iter().enumerate() {
            acc += *n as u64;
            if acc * 2 >= total {
                cut = (k + 1).clamp(1, b.len() - 1);
                break;
            }
        }
        let high = b.split_off(cut);
        boxes.push(b);
        boxes.push(high);
    }
    boxes
        .iter()
        .map(|b| {
            let total: u64 = b.iter().map(|(_, n)| *n as u64).sum::<u64>().max(1);
            let mean = |ch: usize| (b.iter().map(|(c, n)| c[ch] as u64 * *n as u64).sum::<u64>() / total) as u8;
            [mean(0), mean(1), mean(2)]
        })
        .collect()
}

/// The blob for `frames` animation frames (taken evenly from `source`) in a `w` x `h` box, mirrored when `mirror`, then
/// turned clockwise by `tilt` degrees about the middle of the box.
pub fn blob(source: &[Image], w: usize, h: usize, frames: usize, tilt: i32, mirror: bool) -> Vec<u8> {
    // The part of the picture that is ever drawn, the same for every frame so the sprite does not jump.
    let (mut x0, mut y0, mut x1, mut y1) = (usize::MAX, usize::MAX, 0, 0);
    for img in source {
        for y in 0..img.h {
            for x in 0..img.w {
                if img.px[y * img.w + x][3] >= 128 {
                    (x0, y0, x1, y1) = (x0.min(x), y0.min(y), x1.max(x + 1), y1.max(y + 1));
                }
            }
        }
    }
    let (bw, bh) = if x1 > x0 { (x1 - x0, y1 - y0) } else { (1, 1) };
    let inner = (w.min(h) as f64 * FILL).floor().max(1.0);
    let scale = (inner / bw as f64).min(inner / bh as f64);
    let (sw, sh) = (((bw as f64 * scale) as usize).max(1), ((bh as f64 * scale) as usize).max(1));
    let (ox, oy) = ((w - sw.min(w)) / 2, (h - sh.min(h)) / 2);
    let bases: Vec<Image> = (0..frames)
        .map(|f| {
            let src = &source[f * source.len() / frames];
            let mut base = Image::blank(w, h);
            for y in 0..sh.min(h) {
                for x in 0..sw.min(w) {
                    base.px[(oy + y) * w + ox + x] = src.get((x0 + (x as f64 / scale) as usize) as i64, (y0 + (y as f64 / scale) as usize) as i64);
                }
            }
            base
        })
        .collect();
    // Mirrored, turned about the middle of the box, nearest pixel (so no new colours come up).
    let (cx, cy) = ((w as f64 - 1.0) / 2.0, (h as f64 - 1.0) / 2.0);
    let (sin, cos) = (tilt as f64).to_radians().sin_cos();
    let turned: Vec<Vec<Option<[u8; 3]>>> = bases
        .iter()
        .map(|base| {
            (0..w * h)
                .map(|i| {
                    let (dx, dy) = ((i % w) as f64 - cx, (i / w) as f64 - cy);
                    let (mut sx, sy) = (dx * cos + dy * sin + cx, -dx * sin + dy * cos + cy);
                    if mirror {
                        sx = w as f64 - 1.0 - sx;
                    }
                    let p = base.get(sx.round() as i64, sy.round() as i64);
                    (p[3] >= 128).then_some([p[0], p[1], p[2]])
                })
                .collect()
        })
        .collect();
    let mut counts: HashMap<[u8; 3], u32> = HashMap::new();
    for c in turned.iter().flatten().flatten() {
        *counts.entry(*c).or_default() += 1;
    }
    let colours = if counts.len() <= 15 { counts.keys().copied().collect() } else { palette(&counts, 15) };
    let mut nearest: HashMap<[u8; 3], u8> = HashMap::new();
    let mut out = Vec::with_capacity(44 + frames * h * w.div_ceil(2));
    out.extend_from_slice(b"STR2");
    out.extend_from_slice(&(w as u16).to_le_bytes());
    out.extend_from_slice(&(h as u16).to_le_bytes());
    out.extend_from_slice(&[frames as u8, 1, 0, 0]);
    out.extend_from_slice(&[0, 0]);
    for i in 0..15 {
        out.extend_from_slice(&colours.get(i).map_or(0, |c| rgb565(*c)).to_le_bytes());
    }
    let mut index = |c: &Option<[u8; 3]>| -> u8 {
        c.map_or(0, |c| *nearest.entry(c).or_insert_with(|| {
            let d = |p: &[u8; 3]| (0..3).map(|k| (p[k] as i32 - c[k] as i32).pow(2)).sum::<i32>();
            1 + colours.iter().enumerate().min_by_key(|(_, p)| d(p)).map_or(0, |(k, _)| k as u8)
        }))
    };
    for frame in &turned {
        for row in frame.chunks(w) {
            for pair in row.chunks(2) {
                let high = index(&pair[0]);
                let low = pair.get(1).map_or(0, &mut index);
                out.push(high << 4 | low);
            }
        }
    }
    out
}

fn fetch(url: &str) -> Result<Vec<u8>, String> {
    let mut bytes = Vec::new();
    ureq::get(url).timeout(Duration::from_secs(10)).call().map_err(|e| e.to_string())?.into_reader().take(2_000_000).read_to_end(&mut bytes).map_err(|e| e.to_string())?;
    Ok(bytes)
}

fn allowed(url: &str) -> bool {
    let extra = std::env::var("SMALLTV_IMG_ALLOW").unwrap_or_default();
    std::iter::once(DEFAULT_ALLOW).chain(extra.split(',').filter(|p| !p.is_empty())).any(|p| url.starts_with(p))
}

/// What a request target "/sprite?s=48x48&f=6&a=-30&m=1&u=<url>" asks for.
pub struct Request {
    pub w: usize,
    pub h: usize,
    pub frames: usize,
    pub tilt: i32,
    pub mirror: bool,
    pub url: String,
}

pub fn parse_request(target: &str) -> Option<Request> {
    let query = target.strip_prefix("/sprite?")?;
    let (head, url) = query.split_once("u=")?;
    let (mut size, mut frames, mut tilt, mut mirror) = (None, 1usize, 0i32, false);
    for part in head.split('&').filter(|p| !p.is_empty()) {
        match part.split_once('=')? {
            ("s", v) => size = v.split_once('x').and_then(|(a, b)| Some((a.parse::<usize>().ok()?, b.parse::<usize>().ok()?))),
            ("f", v) => frames = v.parse().ok()?,
            ("a", v) => tilt = v.parse().ok().filter(|a| (-90..=90).contains(a))?,
            ("m", v) => mirror = v == "1",
            _ => return None,
        }
    }
    let (w, h) = size?;
    ((8..=240).contains(&w) && (8..=240).contains(&h) && (1..=16).contains(&frames)).then(|| Request { w, h, frames, tilt, mirror, url: url.to_string() })
}

fn reply(stream: &mut TcpStream, status: &str, body: &[u8]) {
    let head = format!("HTTP/1.0 {status}\r\nContent-Type: application/octet-stream\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len());
    let _ = stream.write_all(head.as_bytes()).and_then(|_| stream.write_all(body));
}

fn handle(mut stream: TcpStream, device: Option<IpAddr>, cache: &Mutex<HashMap<String, Arc<Vec<u8>>>>) {
    let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
    let peer = stream.peer_addr().map(|a| a.ip()).ok();
    let permitted = match (peer, device) {
        (Some(p), Some(d)) => p == d || p.is_loopback(),
        (Some(IpAddr::V4(p)), None) => p.is_private() || p.is_loopback(),
        _ => false,
    };
    if !permitted {
        return reply(&mut stream, "403 Forbidden", b"only the clock may ask");
    }
    let mut buf = [0u8; 4096];
    let mut len = 0;
    while !buf[..len].windows(4).any(|w| w == b"\r\n\r\n") && len < buf.len() {
        match stream.read(&mut buf[len..]) {
            Ok(0) | Err(_) => break,
            Ok(n) => len += n,
        }
    }
    let line = String::from_utf8_lossy(&buf[..len]).lines().next().unwrap_or("").to_string();
    let target = line.strip_prefix("GET ").and_then(|r| r.split(' ').next()).unwrap_or("");
    let Some(Request { w, h, frames, tilt, mirror, url }) = parse_request(target) else { return reply(&mut stream, "400 Bad Request", b"bad request") };
    if !allowed(&url) {
        return reply(&mut stream, "403 Forbidden", b"image address not allowed");
    }
    eprintln!("Sprite {} for {}", url.rsplit('/').next().unwrap_or(""), peer.map_or("?".into(), |p| p.to_string()));
    let cached = cache.lock().unwrap().get(target).cloned();
    let body = match cached {
        Some(b) => b,
        None => match fetch(&url).and_then(|bytes| decode(&bytes)).map(|images| Arc::new(blob(&images, w, h, frames, tilt, mirror))) {
            Ok(b) => {
                let mut held = cache.lock().unwrap();
                if held.len() >= 300 {
                    held.clear(); // a few MB at most: sprites are made again when asked
                }
                held.insert(target.to_string(), b.clone());
                b
            }
            Err(e) => return reply(&mut stream, "502 Bad Gateway", e.as_bytes()),
        },
    };
    reply(&mut stream, "200 OK", &body);
}

pub fn serve(device: Option<IpAddr>) {
    let listener = match TcpListener::bind(("0.0.0.0", PORT)) {
        Ok(l) => l,
        Err(e) => return eprintln!("Sprite proxy not started: {e}"),
    };
    let cache = Arc::new(Mutex::new(HashMap::new()));
    std::thread::spawn(move || {
        for stream in listener.incoming().flatten() {
            let cache = cache.clone();
            std::thread::spawn(move || handle(stream, device, &cache));
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bar() -> Image {
        // 20 x 4 opaque red bar on a transparent 24 x 12 picture
        let mut img = Image::blank(24, 12);
        for y in 4..8 {
            for x in 2..22 {
                img.px[y * 24 + x] = [255, 0, 0, 255];
            }
        }
        img
    }

    /// The colour (None = transparent) of every pixel of one frame of a STR2 blob.
    fn colours(blob: &[u8], w: usize, h: usize, frame: usize) -> Vec<Option<u16>> {
        let palette: Vec<u16> = (0..16).map(|i| u16::from_le_bytes([blob[12 + 2 * i], blob[13 + 2 * i]])).collect();
        let stride = w.div_ceil(2);
        (0..w * h)
            .map(|i| {
                let (x, y) = (i % w, i / w);
                let byte = blob[44 + (frame * h + y) * stride + x / 2];
                let v = if x % 2 == 0 { byte >> 4 } else { byte & 15 } as usize;
                (v != 0).then(|| palette[v])
            })
            .collect()
    }

    fn pixels(blob: &[u8], w: usize, h: usize, frame: usize) -> Vec<bool> {
        colours(blob, w, h, frame).iter().map(|c| c.is_some()).collect()
    }

    fn extent(opaque: &[bool], w: usize) -> (usize, usize) {
        let xs: Vec<usize> = opaque.iter().enumerate().filter(|(_, &o)| o).map(|(i, _)| i % w).collect();
        let ys: Vec<usize> = opaque.iter().enumerate().filter(|(_, &o)| o).map(|(i, _)| i / w).collect();
        (xs.iter().max().unwrap() - xs.iter().min().unwrap() + 1, ys.iter().max().unwrap() - ys.iter().min().unwrap() + 1)
    }

    #[test]
    fn blob_has_header_palette_and_exact_size() {
        let b = blob(&[bar()], 41, 40, 3, 0, false);
        assert_eq!(&b[..4], b"STR2");
        assert_eq!((u16::from_le_bytes([b[4], b[5]]), u16::from_le_bytes([b[6], b[7]]), b[8], b[9]), (41, 40, 3, 1));
        assert_eq!(b.len(), 44 + 3 * 40 * 21); // rows of 41 pixels are 21 bytes
        assert_eq!(u16::from_le_bytes([b[12], b[13]]), 0, "index 0 is transparent");
    }

    #[test]
    fn many_colours_are_cut_down_to_15_and_the_picture_stays_recognisable() {
        let mut img = Image::blank(24, 12);
        for y in 0..12 {
            for x in 0..24 {
                img.px[y * 24 + x] = [(x * 10) as u8, (y * 20) as u8, (x * y) as u8, 255]; // a smooth gradient: 288 colours
            }
        }
        let b = blob(&[img.clone()], 40, 40, 1, 0, false);
        let c = colours(&b, 40, 40, 0);
        let used: std::collections::HashSet<u16> = c.iter().flatten().copied().collect();
        assert!(used.len() <= 15 && used.len() >= 8, "{} colours", used.len());
        // the top-left of the picture stays dark and the bottom-right stays bright
        let lum = |c: u16| ((c >> 11) as u32 * 8) + (((c >> 5) & 63) as u32 * 4) + ((c & 31) as u32 * 8);
        let first = c.iter().flatten().next().unwrap();
        let last = c.iter().flatten().last().unwrap();
        assert!(lum(*first) < lum(*last));
    }

    #[test]
    fn the_picture_turns_by_the_tilt_and_stays_inside_the_box() {
        let flat = extent(&pixels(&blob(&[bar()], 40, 40, 1, 0, false), 40, 40, 0), 40);
        let down = extent(&pixels(&blob(&[bar()], 40, 40, 1, 90, false), 40, 40, 0), 40);
        let up = extent(&pixels(&blob(&[bar()], 40, 40, 1, -90, false), 40, 40, 0), 40);
        assert!(flat.0 > 2 * flat.1, "wide when level: {flat:?}");
        assert!(down.1 > 2 * down.0, "tall when going down: {down:?}");
        assert!(down.1.abs_diff(up.1) + down.0.abs_diff(up.0) <= 2);
        for tilt in [-90, -45, -15, 0, 15, 45, 90] {
            let o = pixels(&blob(&[bar()], 40, 40, 1, tilt, false), 40, 40, 0);
            assert!(o.iter().any(|&x| x), "tilt {tilt} is not empty");
            assert!(!o[..40].iter().any(|&x| x) && !o[39 * 40..].iter().any(|&x| x), "tilt {tilt} touches the top or bottom");
        }
    }

    #[test]
    fn mirrored_is_the_mirror_not_turned_over() {
        let mut img = Image::blank(24, 12);
        for y in 5..9 {
            for x in 2..22 {
                img.px[y * 24 + x] = if x < 12 { [255, 0, 0, 255] } else { [0, 0, 255, 255] }; // red head, blue tail
            }
        }
        let leftmost = |mirror: bool| {
            let c = colours(&blob(&[img.clone()], 40, 40, 1, 0, mirror), 40, 40, 0);
            (0..40 * 40).filter(|&i| c[i].is_some()).min_by_key(|i| i % 40).and_then(|i| c[i]).unwrap()
        };
        assert_eq!(leftmost(false), rgb565([255, 0, 0]), "red on the left");
        assert_eq!(leftmost(true), rgb565([0, 0, 255]), "mirrored: blue on the left, still the right way up");
    }

    #[test]
    fn frames_are_taken_evenly() {
        let a = bar();
        let mut c = Image::blank(24, 12);
        c.px[0] = [0, 0, 255, 255];
        let b = blob(&[a, c], 40, 40, 2, 0, false);
        assert_ne!(pixels(&b, 40, 40, 0), pixels(&b, 40, 40, 1));
    }

    #[test]
    fn png_and_gif_decode() {
        let mut png_bytes = Vec::new();
        {
            let mut enc = png::Encoder::new(&mut png_bytes, 2, 2);
            enc.set_color(png::ColorType::Rgba);
            enc.set_depth(png::BitDepth::Eight);
            enc.write_header().unwrap().write_image_data(&[255, 0, 0, 255, 0, 255, 0, 0, 0, 0, 255, 255, 9, 9, 9, 255]).unwrap();
        }
        let png = decode(&png_bytes).unwrap();
        assert_eq!((png.len(), png[0].w, png[0].px[0], png[0].px[1][3]), (1, 2, [255, 0, 0, 255], 0));
        let mut gif_bytes = Vec::new();
        {
            let mut enc = gif::Encoder::new(&mut gif_bytes, 2, 2, &[255, 0, 0, 0, 255, 0]).unwrap();
            for _ in 0..3 {
                let mut f = gif::Frame::default();
                f.width = 2;
                f.height = 2;
                f.buffer = std::borrow::Cow::Owned(vec![0, 1, 1, 0]);
                enc.write_frame(&f).unwrap();
            }
        }
        assert_eq!(decode(&gif_bytes).unwrap().len(), 3);
    }

    #[test]
    fn requests_are_parsed_and_checked() {
        let t = "/sprite?s=48x40&f=6&a=-30&m=1&u=https://raw.githubusercontent.com/PokeAPI/sprites/master/x/25.gif";
        let r = parse_request(t).unwrap();
        assert_eq!((r.w, r.h, r.frames, r.tilt, r.mirror), (48, 40, 6, -30, true));
        assert_eq!(r.url, "https://raw.githubusercontent.com/PokeAPI/sprites/master/x/25.gif");
        assert_eq!(parse_request("/sprite?s=48x40&u=x").map(|r| (r.frames, r.tilt, r.mirror)), Some((1, 0, false)));
        assert!(parse_request("/sprite?s=4x4&f=6&u=x").is_none());
        assert!(parse_request("/sprite?s=48x40&a=120&u=x").is_none());
        assert!(parse_request("/sprite?f=6&u=x").is_none());
        assert!(parse_request("/other?s=48x40&u=x").is_none());
        assert!(allowed("https://raw.githubusercontent.com/PokeAPI/sprites/master/a.png"));
        assert!(!allowed("https://example.com/a.png"));
        assert!(!allowed("http://192.168.0.1/admin"));
    }
}
