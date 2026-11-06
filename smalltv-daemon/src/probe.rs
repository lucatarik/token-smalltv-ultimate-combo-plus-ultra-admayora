//! Incoming connection attempts seen on the wire (AF_PACKET, needs CAP_NET_RAW): a TCP SYN from the internet, to any
//! port, open or closed. /proc cannot show these: a probe of a closed port leaves no socket.
use std::collections::VecDeque;
use std::net::{Ipv4Addr, Ipv6Addr};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

const KEEP: Duration = Duration::from_secs(5); // long enough for the 4 Hz reader to see an attempt
const WIDTH: usize = 20;

/// "<ip:port" for an inbound SYN (no ACK) from a non-LAN address, "u..." for an inbound UDP datagram; None otherwise.
/// `ipv6` tells the header kind (the packet starts at the IP header).
pub fn inspect(ipv6: bool, p: &[u8]) -> Option<String> {
    let (proto, src, l4) = if ipv6 {
        if p.len() < 40 {
            return None;
        }
        (p[6], Ipv6Addr::from(<[u8; 16]>::try_from(&p[8..24]).ok()?).to_string(), &p[40..])
    } else {
        if p.len() < 20 {
            return None;
        }
        let ihl = (p[0] & 15) as usize * 4;
        (p[9], Ipv4Addr::new(p[12], p[13], p[14], p[15]).to_string(), p.get(ihl..)?)
    };
    if crate::net::private(&src) || src == "::" || src == "::1" || src.starts_with("127.") || src.starts_with("ff") {
        return None;
    }
    let mark = match proto {
        6 if l4.len() >= 14 && l4[13] & 0x12 == 0x02 => '<', // SYN without ACK
        17 if l4.len() >= 8 => 'u',
        _ => return None,
    };
    let port = u16::from_be_bytes([l4[2], l4[3]]).to_string(); // the port it was sent to
    if mark == 'u' && matches!(u16::from_be_bytes([l4[2], l4[3]]), 53 | 123 | 5353 | 41641) {
        return None;
    }
    let room = WIDTH - 1 - port.len() - 1;
    Some(format!("{mark}{}:{port}", &src[..src.len().min(room)]))
}

/// The attempts of the last few seconds. Starts a reader thread; without CAP_NET_RAW it says so once and stays empty.
#[derive(Clone, Default)]
pub struct Probes(Arc<Mutex<VecDeque<(Instant, String)>>>);

impl Probes {
    pub fn start() -> Probes {
        let probes = Probes::default();
        let held = probes.0.clone();
        std::thread::spawn(move || {
            // SOCK_DGRAM: the kernel strips the link header, so Ethernet and tun interfaces (tailscale) look alike.
            let fd = unsafe { libc::socket(libc::AF_PACKET, libc::SOCK_DGRAM, (libc::ETH_P_ALL as u16).to_be() as i32) };
            if fd < 0 {
                eprintln!("Probe capture off: {} (grant it with: sudo setcap cap_net_raw+ep ~/.local/bin/smalltv-monitord)", std::io::Error::last_os_error());
                return;
            }
            let mut buf = [0u8; 256];
            loop {
                let mut from: libc::sockaddr_ll = unsafe { std::mem::zeroed() };
                let mut len = std::mem::size_of::<libc::sockaddr_ll>() as libc::socklen_t;
                let n = unsafe { libc::recvfrom(fd, buf.as_mut_ptr().cast(), buf.len(), 0, (&mut from as *mut libc::sockaddr_ll).cast(), &mut len) };
                if n <= 0 || from.sll_pkttype == libc::PACKET_OUTGOING {
                    continue;
                }
                let kind = u16::from_be(from.sll_protocol);
                if kind != libc::ETH_P_IP as u16 && kind != libc::ETH_P_IPV6 as u16 {
                    continue;
                }
                if let Some(c) = inspect(kind == libc::ETH_P_IPV6 as u16, &buf[..n as usize]) {
                    let mut q = held.lock().unwrap();
                    q.retain(|(_, x)| *x != c);
                    q.push_back((Instant::now(), c));
                    if q.len() > 64 {
                        q.pop_front();
                    }
                }
            }
        });
        probes
    }

    pub fn recent(&self) -> Vec<String> {
        let mut q = self.0.lock().unwrap();
        q.retain(|(t, _)| t.elapsed() < KEEP);
        q.iter().map(|(_, c)| c.clone()).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v4(proto: u8, src: [u8; 4], l4: &[u8]) -> Vec<u8> {
        let mut p = vec![0x45, 0, 0, 0, 0, 0, 0, 0, 64, proto, 0, 0];
        p.extend(src);
        p.extend([192, 168, 0, 5]);
        p.extend(l4);
        p
    }

    #[test]
    fn syn_from_the_internet_is_listed_with_the_port_it_hit() {
        let mut tcp = vec![0xC3, 0x50, 0x00, 0x16, 0, 0, 0, 0, 0, 0, 0, 0, 0x50, 0x02];
        tcp.extend([0; 6]);
        assert_eq!(inspect(false, &v4(6, [45, 9, 1, 2], &tcp)), Some("<45.9.1.2:22".into()));
    }

    #[test]
    fn lan_ack_and_dns_are_ignored() {
        let mut tcp = vec![0xC3, 0x50, 0x00, 0x16, 0, 0, 0, 0, 0, 0, 0, 0, 0x50, 0x12];
        tcp.extend([0; 6]);
        assert_eq!(inspect(false, &v4(6, [45, 9, 1, 2], &tcp)), None); // SYN+ACK: a reply, not an attempt
        tcp[13] = 0x02;
        assert_eq!(inspect(false, &v4(6, [192, 168, 0, 9], &tcp)), None);
        assert_eq!(inspect(false, &v4(17, [45, 9, 1, 2], &[0, 53, 0, 53, 0, 8, 0, 0])), None);
        assert_eq!(inspect(false, &v4(17, [45, 9, 1, 2], &[0, 53, 0x1F, 0x90, 0, 8, 0, 0])), Some("u45.9.1.2:8080".into()));
    }
}
