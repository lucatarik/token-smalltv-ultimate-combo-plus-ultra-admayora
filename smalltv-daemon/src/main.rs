//! smalltv-monitord: pushes Claude's limits, Claude's status and this computer's load to a SmallTV running
//! smalltv-mod (POST /api/usage, once a second). Run it by hand, or switch it on and off as a systemd user
//! service:
//!   smalltv-monitord install --device-url http://192.168.0.29   # on, and on at every login
//!   smalltv-monitord uninstall                                  # off, and not at login
//!   smalltv-monitord status
//!   smalltv-monitord run --device-url http://192.168.0.29       # in the foreground
//! Also pushes the last non-HTTP connections (/api/net) and the weather (/api/wx). The place comes from
//! SMALLTV_LAT, SMALLTV_LON and SMALLTV_PLACE (default Cecchina).
mod claude;
mod metrics;
mod net;
mod probe;
mod sprites;
mod weather;

use serde_json::{json, Map, Value};
use std::path::PathBuf;
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const PUSH_EVERY: Duration = Duration::from_secs(1);
const CLAUDE_EVERY: Duration = Duration::from_secs(300); // Claude's usage endpoint answers frequent callers with 429
const WEATHER_EVERY: Duration = Duration::from_secs(600);
const CONNECTIONS_EVERY: Duration = Duration::from_millis(250);
const UNIT: &str = "smalltv-monitord.service";

#[derive(Default)]
struct Shared {
    limits: Option<claude::Limits>,
    status: Option<&'static str>,
    weather: Option<Value>,
    connections: Vec<String>,
}

fn place() -> (f64, f64, String) {
    let num = |k: &str, d: f64| std::env::var(k).ok().and_then(|v| v.parse().ok()).unwrap_or(d);
    (num("SMALLTV_LAT", 41.75), num("SMALLTV_LON", 12.64), std::env::var("SMALLTV_PLACE").unwrap_or_else(|_| "Cecchina".into()))
}

/// Looks at the sockets four times a second, so a connection that lives for a moment is still seen.
fn spawn_connections(shared: Arc<Mutex<Shared>>) {
    std::thread::spawn(move || {
        let mut recent = net::Recent::default();
        let probes = probe::Probes::start();
        loop {
            let mut now = net::connections();
            now.extend(probes.recent());
            let list = recent.update(now);
            shared.lock().unwrap().connections = list;
            std::thread::sleep(CONNECTIONS_EVERY);
        }
    });
}

/// The weather, read every ten minutes in the background.
fn spawn_weather(shared: Arc<Mutex<Shared>>) {
    std::thread::spawn(move || {
        let (lat, lon, name) = place();
        loop {
            match weather::fetch(lat, lon).map(|api| weather::payload(&name, &api)) {
                Ok(Some(body)) => shared.lock().unwrap().weather = Some(body),
                Ok(None) => eprintln!("Weather answer not understood"),
                Err(error) => eprintln!("Weather not read: {error}"),
            }
            std::thread::sleep(WEATHER_EVERY);
        }
    });
}

fn now_secs() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

fn minutes_left(window: Option<&claude::Window>, now: i64) -> i64 {
    window.and_then(|w| w.resets_at).map_or(0, |at| ((at - now + 59) / 60).max(0))
}

/// The JSON the firmware takes; a value that is not known is left out, so the clock draws a dash.
fn payload(limits: Option<&claude::Limits>, status: Option<&str>, reading: &metrics::Reading, now: i64) -> Value {
    let mut body = Map::new();
    match limits.filter(|l| l.five.is_some() || l.week.is_some()) {
        Some(l) => {
            body.insert("s".into(), json!(l.five.as_ref().map_or(0, |w| w.used)));
            body.insert("sr".into(), json!(minutes_left(l.five.as_ref(), now)));
            body.insert("w".into(), json!(l.week.as_ref().map_or(0, |w| w.used)));
            body.insert("wr".into(), json!(minutes_left(l.week.as_ref(), now)));
            body.insert("ok".into(), json!(true));
        }
        None => {
            body.insert("ok".into(), json!(false));
        }
    }
    for (key, value) in [
        ("cpu", reading.cpu.map(u64::from)),
        ("ram", reading.ram.map(u64::from)),
        ("dk", reading.disk.map(u64::from)),
        ("dn", reading.down),
        ("up", reading.up),
    ] {
        if let Some(value) = value {
            body.insert(key.into(), json!(value));
        }
    }
    body.insert("cs".into(), json!(status.unwrap_or("")));
    Value::Object(body)
}

fn claude_home() -> PathBuf {
    PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".claude")
}

/// Claude's limits and status, read in the background so a slow answer never delays the pushes.
fn spawn_claude(shared: Arc<Mutex<Shared>>) {
    std::thread::spawn(move || {
        let mut complained: Option<String> = None;
        loop {
            match claude::fetch_usage(&claude_home()) {
                Ok(limits) => {
                    shared.lock().unwrap().limits = Some(limits);
                    complained = None;
                }
                Err(code) => {
                    if complained.as_deref() != Some(&code) {
                        eprintln!("Claude usage not read: {code}"); // said once, not every five minutes
                        complained = Some(code);
                    }
                }
            }
            shared.lock().unwrap().status = claude::fetch_status();
            std::thread::sleep(CLAUDE_EVERY);
        }
    });
}

fn run(url: &str) {
    let endpoint = format!("{}/api/usage", url.trim_end_matches('/'));
    let shared = Arc::new(Mutex::new(Shared::default()));
    spawn_claude(shared.clone());
    spawn_weather(shared.clone());
    spawn_connections(shared.clone());
    sprites::serve(url.trim_start_matches("http://").trim_start_matches("https://").split(['/', ':']).next().and_then(|h| h.parse().ok()));
    let mut machine = metrics::Machine::new();
    let mut sent_connections: Vec<String> = Vec::new();
    let mut failed = false;
    for tick in 0u64.. {
        let side = |path: &str, body: Value| {
            let _ = ureq::post(&format!("{}{path}", url.trim_end_matches('/'))).timeout(Duration::from_secs(3)).send_json(body);
        };
        let started = Instant::now();
        let reading = machine.sample();
        let body = {
            let held = shared.lock().unwrap();
            payload(held.limits.as_ref(), held.status, &reading, now_secs())
        };
        match ureq::post(&endpoint).timeout(Duration::from_secs(3)).send_json(body) {
            Ok(_) => {
                if failed {
                    eprintln!("Clock at {url} answers again");
                }
                failed = false;
            }
            Err(error) => {
                if !failed {
                    eprintln!("Clock at {url} not answering: {error}"); // said once, not every second
                }
                failed = true;
            }
        }
        let connections = shared.lock().unwrap().connections.clone();
        if connections != sent_connections || tick % 5 == 1 {   // when the list moves, at once; else now and then
            side("/api/net", json!({"c": connections}));
            sent_connections = connections;
        }
        if tick % 60 == 5 {
            if let Some(body) = shared.lock().unwrap().weather.clone() {
                side("/api/wx", body);
            }
        }
        std::thread::sleep(PUSH_EVERY.saturating_sub(started.elapsed()));
    }
}

fn systemctl(args: &[&str]) -> bool {
    Command::new("systemctl").arg("--user").args(args).status().map(|s| s.success()).unwrap_or(false)
}

fn unit_path() -> PathBuf {
    let base = std::env::var("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".config"));
    base.join("systemd/user").join(UNIT)
}

fn install(url: &str) -> Result<(), String> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let unit = format!(
        "[Unit]\nDescription=SmallTV monitor daemon\nAfter=network-online.target\n\n\
         [Service]\nExecStart={} run --device-url {}\nRestart=on-failure\nRestartSec=5\n\n\
         [Install]\nWantedBy=default.target\n",
        exe.display(),
        url
    );
    let path = unit_path();
    std::fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
    std::fs::write(&path, unit).map_err(|e| e.to_string())?;
    if systemctl(&["daemon-reload"]) && systemctl(&["enable", "--now", UNIT]) {
        println!("On: {UNIT} runs now and at every login. Off: smalltv-monitord uninstall");
        Ok(())
    } else {
        Err("systemctl --user failed".into())
    }
}

fn uninstall() -> Result<(), String> {
    systemctl(&["disable", "--now", UNIT]);
    match std::fs::remove_file(unit_path()) {
        Ok(()) => {}
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => return Err(e.to_string()),
    }
    systemctl(&["daemon-reload"]);
    println!("Off: {UNIT} stopped and removed.");
    Ok(())
}

fn device_url(args: &[String]) -> Result<String, String> {
    let url = args
        .windows(2)
        .find(|w| w[0] == "--device-url")
        .map(|w| w[1].clone())
        .or_else(|| std::env::var("SMALLTV_URL").ok())
        .ok_or("pass --device-url http://<ip> (or set SMALLTV_URL)")?;
    if url.starts_with("http://") || url.starts_with("https://") {
        Ok(url)
    } else {
        Err(format!("the clock address must start with http://: {url}"))
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result = match args.first().map(String::as_str) {
        Some("run") => device_url(&args).map(|url| run(&url)),
        Some("install") => device_url(&args).and_then(|url| install(&url)),
        Some("uninstall") => uninstall(),
        Some("connections") => {
            net::connections().iter().for_each(|c| println!("{c}"));
            Ok(())
        }
        Some("status") => {
            systemctl(&["status", "--no-pager", UNIT]);
            Ok(())
        }
        _ => Err("usage: smalltv-monitord run|install|uninstall|status [--device-url http://<ip>]".into()),
    };
    if let Err(message) = result {
        eprintln!("{message}");
        std::process::exit(2);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use claude::{Limits, Window};

    #[test]
    fn payload_has_limits_computer_and_status() {
        let limits = Limits {
            five: Some(Window { used: 41, resets_at: Some(1000 + 7200) }),
            week: Some(Window { used: 86, resets_at: Some(1000 + 259_200) }),
        };
        let reading = metrics::Reading { cpu: Some(27), ram: Some(58), disk: Some(63), down: Some(1_300_000), up: Some(84_000) };
        let body = payload(Some(&limits), Some("ok"), &reading, 1000);
        assert_eq!(body, json!({"s": 41, "sr": 120, "w": 86, "wr": 4320, "ok": true, "cpu": 27, "ram": 58,
                                "dk": 63, "dn": 1_300_000, "up": 84_000, "cs": "ok"}));
    }

    #[test]
    fn unknown_values_are_left_out_not_zero() {
        let body = payload(None, None, &metrics::Reading::default(), 1000);
        assert_eq!(body, json!({"ok": false, "cs": ""}));
    }

    #[test]
    fn device_url_must_be_http() {
        let args = |a: &[&str]| a.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert!(device_url(&args(&["run", "--device-url", "192.168.0.29"])).is_err());
        assert_eq!(device_url(&args(&["run", "--device-url", "http://192.168.0.29"])).unwrap(), "http://192.168.0.29");
    }
}
