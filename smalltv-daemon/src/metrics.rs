//! This computer's load, read from /proc and statvfs. An unreadable value is None, never 0.
use std::fs;
use std::time::Instant;

#[derive(Debug, Default, PartialEq)]
pub struct Reading {
    pub cpu: Option<u8>,
    pub ram: Option<u8>,
    pub disk: Option<u8>,
    pub down: Option<u64>, // bytes per second
    pub up: Option<u64>,
}

pub struct Machine {
    cpu: Option<(u64, u64)>,
    net: Option<(Instant, u64, u64)>,
}

impl Machine {
    pub fn new() -> Self {
        let mut machine = Machine { cpu: None, net: None };
        machine.sample(); // the counters' starting point: the first reading has no CPU or speed yet
        machine
    }

    pub fn sample(&mut self) -> Reading {
        let cpu_now = fs::read_to_string("/proc/stat").ok().and_then(|t| parse_cpu(&t));
        let cpu = match (self.cpu, cpu_now) {
            (Some((busy0, total0)), Some((busy, total))) if total > total0 => {
                Some((100 * (busy - busy0) / (total - total0)) as u8)
            }
            _ => None,
        };
        self.cpu = cpu_now;

        let net_now = fs::read_to_string("/proc/net/dev").ok().and_then(|t| parse_net(&t));
        let at = Instant::now();
        let (mut down, mut up) = (None, None);
        if let (Some((then, down0, up0)), Some((d, u))) = (self.net, net_now) {
            let span = at.duration_since(then).as_secs_f64();
            if span > 0.0 {
                down = Some((d.saturating_sub(down0) as f64 / span) as u64);
                up = Some((u.saturating_sub(up0) as f64 / span) as u64);
            }
        }
        self.net = net_now.map(|(d, u)| (at, d, u));

        Reading {
            cpu,
            ram: fs::read_to_string("/proc/meminfo").ok().and_then(|t| parse_meminfo(&t)),
            disk: disk(),
            down,
            up,
        }
    }
}

/// (busy, total) jiffies of all CPUs from /proc/stat; idle and iowait are not busy.
pub fn parse_cpu(text: &str) -> Option<(u64, u64)> {
    let line = text.lines().next()?;
    let numbers: Vec<u64> = line.strip_prefix("cpu ")?.split_whitespace().filter_map(|n| n.parse().ok()).collect();
    if numbers.len() < 5 {
        return None;
    }
    let total: u64 = numbers.iter().take(8).sum();
    Some((total - numbers[3] - numbers[4], total))
}

/// Percent of RAM in use: what is not available to programs.
pub fn parse_meminfo(text: &str) -> Option<u8> {
    let field = |name: &str| -> Option<u64> {
        text.lines().find_map(|l| l.strip_prefix(name)?.strip_prefix(':')?.split_whitespace().next()?.parse().ok())
    };
    let (total, available) = (field("MemTotal")?, field("MemAvailable")?);
    (total > 0).then(|| (100 * total.saturating_sub(available) / total) as u8)
}

/// (received, sent) bytes of every interface but loopback.
pub fn parse_net(text: &str) -> Option<(u64, u64)> {
    let mut found = false;
    let (mut down, mut up) = (0u64, 0u64);
    for line in text.lines().skip(2) {
        let (name, rest) = line.split_once(':')?;
        if name.trim() == "lo" {
            continue;
        }
        let numbers: Vec<u64> = rest.split_whitespace().filter_map(|n| n.parse().ok()).collect();
        if numbers.len() >= 9 {
            down += numbers[0];
            up += numbers[8];
            found = true;
        }
    }
    found.then_some((down, up))
}

/// Percent of the disk holding the home folder that is in use.
fn disk() -> Option<u8> {
    let path = std::ffi::CString::new(std::env::var("HOME").unwrap_or_else(|_| "/".into())).ok()?;
    let mut stat: libc::statvfs = unsafe { std::mem::zeroed() };
    if unsafe { libc::statvfs(path.as_ptr(), &mut stat) } != 0 || stat.f_blocks == 0 {
        return None;
    }
    let (blocks, free) = (stat.f_blocks as u64, stat.f_bfree as u64);
    Some((100 * (blocks - free) / blocks) as u8)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cpu_counts_idle_and_iowait_as_not_busy() {
        let text = "cpu  100 0 50 800 40 5 5 0 0 0\ncpu0 1 1 1 1 1 1 1 1\n";
        assert_eq!(parse_cpu(text), Some((160, 1000)));
        assert_eq!(parse_cpu("nonsense"), None);
    }

    #[test]
    fn ram_is_total_minus_available() {
        let text = "MemTotal:       1000 kB\nMemFree: 100 kB\nMemAvailable:    400 kB\n";
        assert_eq!(parse_meminfo(text), Some(60));
        assert_eq!(parse_meminfo("MemTotal: 1000 kB\n"), None);
    }

    #[test]
    fn net_skips_loopback_and_sums_the_rest() {
        let text = "Inter-|   Receive\n face |bytes\n    lo: 999 1 0 0 0 0 0 0 999 1 0 0 0 0 0 0\n eth0: 100 1 0 0 0 0 0 0 40 1 0 0 0 0 0 0\n wlan0: 50 1 0 0 0 0 0 0 10 1 0 0 0 0 0 0\n";
        assert_eq!(parse_net(text), Some((150, 50)));
    }
}
