//! Claude's limits (the usage endpoint Claude Code itself uses, with the token of its own login) and
//! Claude's service status. The token is read from the CLI's credentials file, sent only to
//! api.anthropic.com, and never printed or kept.
use serde_json::Value;
use std::path::Path;
use std::time::Duration;
use time::format_description::well_known::Rfc3339;
use time::OffsetDateTime;

const USAGE_URL: &str = "https://api.anthropic.com/api/oauth/usage";
const STATUS_URL: &str = "https://status.claude.com/api/v2/status.json";

#[derive(Debug, Clone, PartialEq)]
pub struct Window {
    pub used: u8,
    pub resets_at: Option<i64>, // unix seconds
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct Limits {
    pub five: Option<Window>,
    pub week: Option<Window>,
}

fn timestamp(value: &Value) -> Option<i64> {
    match value {
        Value::Number(n) => n.as_i64().filter(|&n| n > 0),
        Value::String(s) => OffsetDateTime::parse(s, &Rfc3339).ok().map(|t| t.unix_timestamp()),
        _ => None,
    }
}

fn window(value: &Value) -> Option<Window> {
    let used = value.get("utilization")?.as_f64().filter(|u| u.is_finite() && *u >= 0.0)?;
    Some(Window { used: used.round().min(100.0) as u8, resets_at: value.get("resets_at").and_then(timestamp) })
}

pub fn parse_usage(data: &Value) -> Limits {
    Limits { five: data.get("five_hour").and_then(window), week: data.get("seven_day").and_then(window) }
}

pub fn read_token(claude_home: &Path) -> Result<String, String> {
    let text = std::fs::read_to_string(claude_home.join(".credentials.json")).map_err(|_| "auth_required".to_string())?;
    let json: Value = serde_json::from_str(&text).map_err(|_| "auth_required".to_string())?;
    json["claudeAiOauth"]["accessToken"].as_str().map(str::to_owned).ok_or_else(|| "auth_required".to_string())
}

/// The error is a short code (never the reply or the token): `auth_required`, `rate_limited`, `error`.
pub fn fetch_usage(claude_home: &Path) -> Result<Limits, String> {
    let token = read_token(claude_home)?;
    let reply = ureq::get(USAGE_URL)
        .timeout(Duration::from_secs(10))
        .set("Authorization", &format!("Bearer {token}"))
        .set("Accept", "application/json")
        .set("User-Agent", "claude-code/2.1.287")
        .set("anthropic-beta", "oauth-2025-04-20")
        .call();
    match reply {
        Ok(response) => response.into_json::<Value>().map(|v| parse_usage(&v)).map_err(|_| "error".to_string()),
        Err(ureq::Error::Status(401, _)) | Err(ureq::Error::Status(403, _)) => Err("auth_required".into()),
        Err(ureq::Error::Status(429, _)) => Err("rate_limited".into()),
        Err(_) => Err("error".into()),
    }
}

/// The firmware's word for Claude's service status, or None when it could not be read.
pub fn fetch_status() -> Option<&'static str> {
    let reply: Value = ureq::get(STATUS_URL).timeout(Duration::from_secs(5)).call().ok()?.into_json().ok()?;
    status_word(reply["status"]["indicator"].as_str()?)
}

pub fn status_word(indicator: &str) -> Option<&'static str> {
    match indicator {
        "none" => Some("ok"),
        "minor" => Some("minor"),
        "major" => Some("major"),
        "critical" => Some("down"),
        "maintenance" => Some("maint"),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn usage_reads_both_windows_and_iso_resets() {
        let data = json!({"five_hour": {"utilization": 41.4, "resets_at": "2026-11-04T17:00:00+00:00"},
                          "seven_day": {"utilization": 86, "resets_at": 1_800_000_000}});
        let limits = parse_usage(&data);
        assert_eq!(limits.five, Some(Window { used: 41, resets_at: Some(1_793_811_600) }));
        assert_eq!(limits.week, Some(Window { used: 86, resets_at: Some(1_800_000_000) }));
    }

    #[test]
    fn missing_or_odd_windows_are_none_not_zero() {
        let limits = parse_usage(&json!({"five_hour": {"utilization": null}, "seven_day": "x"}));
        assert_eq!(limits, Limits::default());
    }

    #[test]
    fn status_words() {
        assert_eq!(status_word("critical"), Some("down"));
        assert_eq!(status_word("surprise"), None);
    }
}
