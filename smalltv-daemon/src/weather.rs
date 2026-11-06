//! Weather for one place from Open-Meteo (no key), reduced to the few numbers the clock draws.
use serde_json::{json, Value};
use std::time::Duration;

pub fn fetch(lat: f64, lon: f64) -> Result<Value, String> {
    let url = format!(
        "https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}\
         &current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m\
         &daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=4&timezone=auto"
    );
    ureq::get(&url).timeout(Duration::from_secs(10)).call().map_err(|e| e.to_string())?.into_json().map_err(|e| e.to_string())
}

/// Day of the week of a calendar date, 0 = Sunday.
fn weekday(y: i64, m: i64, d: i64) -> i64 {
    const T: [i64; 12] = [0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4];
    let y = if m < 3 { y - 1 } else { y };
    (y + y / 4 - y / 100 + y / 400 + T[(m - 1) as usize] + d) % 7
}

/// The body for the clock's /api/wx; None when the answer lacks a field.
pub fn payload(place: &str, api: &Value) -> Option<Value> {
    let cur = api.get("current")?;
    let daily = api.get("daily")?;
    let mut date = daily["time"][0].as_str()?.split('-').map(|n| n.parse::<i64>().ok());
    let wd = weekday(date.next()??, date.next()??, date.next()??);
    let day = |i: usize| -> Option<Value> {
        Some(json!([daily["temperature_2m_max"][i].as_f64()?.round() as i64, daily["temperature_2m_min"][i].as_f64()?.round() as i64, daily["weather_code"][i].as_u64()?]))
    };
    Some(json!({
        "p": place.chars().take(15).collect::<String>(),
        "t": cur["temperature_2m"].as_f64()?.round() as i64,
        "c": cur["weather_code"].as_u64()?,
        "h": cur["relative_humidity_2m"].as_f64()?.round() as i64,
        "w": cur["wind_speed_10m"].as_f64()?.round() as i64,
        "wd": wd,
        "d": [day(1)?, day(2)?, day(3)?],
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn weekday_known_dates() {
        assert_eq!(weekday(2026, 11, 6), 5); // Friday
        assert_eq!(weekday(2024, 2, 29), 4); // Thursday
        assert_eq!(weekday(2000, 1, 2), 0); // Sunday
    }

    #[test]
    fn payload_takes_next_three_days() {
        let api = json!({
            "current": {"temperature_2m": 17.4, "relative_humidity_2m": 64.0, "weather_code": 3, "wind_speed_10m": 11.6},
            "daily": {"time": ["2026-11-06", "2026-11-07", "2026-11-08", "2026-11-09"],
                      "weather_code": [3, 61, 0, 2], "temperature_2m_max": [19.0, 18.2, 20.0, 21.5],
                      "temperature_2m_min": [10.0, 11.0, 9.0, 8.0]}
        });
        let body = payload("Cecchina", &api).unwrap();
        assert_eq!(body["t"], json!(17));
        assert_eq!(body["wd"], json!(5));
        assert_eq!(body["d"], json!([[18, 11, 61], [20, 9, 0], [22, 8, 2]]));
        assert!(payload("x", &json!({})).is_none());
    }
}
