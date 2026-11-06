//! Suspicious connections, from /proc/net/tcp, tcp6, udp and udp6: anything arriving at a port we listen on, and
//! anything leaving for an unusual port. The newest few are kept. Probes of closed ports leave no socket: not seen.
use std::collections::{HashSet, VecDeque};
use std::net::{Ipv4Addr, Ipv6Addr};

const WEB: [u16; 4] = [80, 443, 8080, 8443];
// Remote TCP ports an ordinary program uses (ssh, dns, mail, ntp, web, ...): an outgoing connection elsewhere is odd.
const TCP_OK: [u16; 15] = [22, 53, 80, 123, 143, 443, 465, 587, 853, 993, 995, 5228, 5222, 8080, 8443];
// Same for UDP (dns, ntp, quic, mdns, ssdp, tailscale).
const UDP_OK: [u16; 6] = [53, 123, 443, 5353, 1900, 41641];
// Our own services the LAN is meant to reach (the clock's sprite proxy).
const OWN: [u16; 1] = [8899];
const SHOWN: usize = 8; // the clock keeps this many and shows the newest that fit
const WIDTH: usize = 20; // characters the clock's row holds

fn hex_addr(hex: &str) -> Option<String> {
    let word = |i: usize| u32::from_str_radix(hex.get(i * 8..i * 8 + 8)?, 16).ok().map(u32::swap_bytes);
    match hex.len() {
        8 => Some(Ipv4Addr::from(word(0)?).to_string()),
        32 => {
            let a = Ipv6Addr::from(((word(0)? as u128) << 96) | ((word(1)? as u128) << 64) | ((word(2)? as u128) << 32) | word(3)? as u128);
            Some(a.to_ipv4_mapped().map_or_else(|| a.to_string(), |v4| v4.to_string()))
        }
        _ => None,
    }
}

pub fn private(ip: &str) -> bool {
    let Ok(v4) = ip.parse::<Ipv4Addr>() else { return ip.starts_with("fd") || ip.starts_with("fe80") };
    let o = v4.octets();
    v4.is_private() || v4.is_link_local() || (o[0] == 100 && (64..128).contains(&o[1]))
}

/// One row per suspicious socket, as "<prefix>ip:port": `<` TCP that came to a port we listen on (from the internet, or
/// from the LAN unless it is web or our own service), `>` TCP we opened to an unusual remote port, `u` UDP with a
/// known peer outside the LAN on an unusual port. Closing sockets (TIME_WAIT, ...) count: they just ended.
pub fn parse(text: &str, udp: bool) -> Vec<String> {
    let rows: Vec<Vec<&str>> = text.lines().skip(1).map(|l| l.split_whitespace().collect()).filter(|f: &Vec<&str>| f.len() >= 4).collect();
    let port = |f: &str| f.split_once(':').and_then(|(_, p)| u16::from_str_radix(p, 16).ok());
    let listening: HashSet<u16> = rows.iter().filter(|f| !udp && f[3] == "0A").filter_map(|f| port(f[1])).collect();
    let mut out = Vec::new();
    for f in rows {
        let (Some((addr, _)), Some(rport), Some(lport)) = (f[2].split_once(':'), port(f[2]), port(f[1])) else { continue };
        let Some(ip) = hex_addr(addr) else { continue };
        let skip = if udp { rport == 0 } else { f[3] == "0A" || f[3] == "07" };
        if skip || ip.starts_with("127.") || ip == "::1" || ip == "0.0.0.0" || ip == "::" {
            continue;
        }
        let lan = private(&ip);
        let mark = if udp {
            if lan || UDP_OK.contains(&rport) { continue }
            'u'
        } else if listening.contains(&lport) {
            if lan && (WEB.contains(&lport) || OWN.contains(&lport)) { continue }
            '<'
        } else {
            if TCP_OK.contains(&rport) { continue }
            '>'
        };
        let port = rport.to_string();
        let room = WIDTH - 1 - port.len() - 1;
        out.push(format!("{mark}{}:{port}", &ip[..ip.len().min(room)]));
    }
    out
}

pub fn connections() -> Vec<String> {
    let mut all = Vec::new();
    for (file, udp) in [("/proc/net/tcp", false), ("/proc/net/tcp6", false), ("/proc/net/udp", true), ("/proc/net/udp6", true)] {
        if let Ok(text) = std::fs::read_to_string(file) {
            all.extend(parse(&text, udp));
        }
    }
    all
}

/// The last few distinct connections, newest first; one that closes stays listed until pushed out.
#[derive(Default)]
pub struct Recent {
    seen: HashSet<String>,
    list: VecDeque<String>,
}

impl Recent {
    pub fn update(&mut self, now: Vec<String>) -> Vec<String> {
        for c in &now {
            if !self.seen.contains(c) {
                self.list.retain(|x| x != c);
                self.list.push_front(c.clone());
            }
        }
        self.seen = now.into_iter().collect();
        self.list.truncate(SHOWN);
        self.list.iter().cloned().collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const TCP: &str = "  sl  local_address rem_address   st\n\
   0: 0100A8C0:0016 00000000:0000 0A 0:0 00:0 0 0\n\
   1: 0100007F:1F90 0100007F:D2A0 01 0:0 00:0 0 0\n\
   2: 0100A8C0:C350 0100A8C0:0016 01 0:0 00:0 0 0\n\
   3: 0100A8C0:C351 5DB8D822:01BB 01 0:0 00:0 0 0\n\
   4: 0100A8C0:0016 0A00A8C0:9C40 01 0:0 00:0 0 0\n\
   5: 0100A8C0:0016 5DB8D822:9C41 03 0:0 00:0 0 0\n\
   6: 0100A8C0:C354 0B00A8C0:2710 06 0:0 00:0 0 0\n\
   7: 0100A8C0:C355 0B00A8C0:0050 06 0:0 00:0 0 0\n";

    #[test]
    fn tcp_keeps_odd_out_and_incoming_not_web_or_loopback() {
        assert_eq!(parse(TCP, false), vec!["<192.168.0.10:40000", "<34.216.184.93:40001", ">192.168.0.11:10000"]);
    }

    #[test]
    fn udp_needs_a_public_peer_on_an_unusual_port() {
        let t = "h\n 0: 0100A8C0:C350 5DB8D822:1388 01 0:0\n 1: 0100A8C0:C351 00000000:0000 07 0:0\n 2: 0100A8C0:C352 5DB8D822:0035 01 0:0\n 3: 0100A8C0:C353 0100A8C0:1388 01 0:0\n";
        assert_eq!(parse(t, true), vec!["u34.216.184.93:5000"]);
    }

    #[test]
    fn mapped_ipv6_shows_as_ipv4() {
        let t = "h\n 0: 00000000000000000000000000000000:C350 0000000000000000FFFF00000100A8C0:2710 01 0:0\n";
        assert_eq!(parse(t, false), vec![">192.168.0.1:10000"]);
    }

    #[test]
    fn recent_lists_new_first_and_keeps_closed() {
        let mut r = Recent::default();
        assert_eq!(r.update(vec!["a:1".into()]), vec!["a:1"]);
        assert_eq!(r.update(vec!["a:1".into(), "b:2".into()]), vec!["b:2", "a:1"]);
        assert_eq!(r.update(vec![]), vec!["b:2", "a:1"]);
        let many: Vec<String> = (0..9).map(|i| format!("h{i}:1")).collect();
        assert_eq!(r.update(many).len(), SHOWN);
    }
}
