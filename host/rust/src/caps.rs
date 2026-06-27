//! Parse `SYSTem:CAPabilities?` responses.
//!
//! Current firmware returns a semicolon-separated key=value string shaped like:
//!
//! ```text
//! proto=1;mtu=256;maxblock=4096;feat=
//! ```
//!
//! The parser is deliberately tolerant:
//! - unknown keys are retained in [`Capabilities::unknown`]
//! - missing keys use `None` / empty defaults
//! - numeric parse failures (e.g. some toolchains print `%zu` literally as
//!   `zu`) are recorded in [`Capabilities::parse_errors`] rather than aborting
//!   the whole parse, so a slightly-broken capabilities string still yields
//!   useful structured data.

use std::collections::BTreeMap;

/// Parsed device capabilities.
#[derive(Debug, Clone, Default)]
pub struct Capabilities {
    pub proto: Option<u32>,
    pub mtu: Option<usize>,
    pub max_block: Option<usize>,
    pub features: Vec<String>,
    /// Keys not understood by this version of the host, preserved verbatim.
    pub unknown: BTreeMap<String, String>,
    /// Numeric fields whose value could not be parsed: `(key, raw_value)`.
    pub parse_errors: Vec<(String, String)>,
    /// The original response text.
    pub raw: String,
}

/// Parse a capabilities response. Never fails: malformed numeric fields are
/// recorded in [`Capabilities::parse_errors`] and the corresponding field is
/// left as `None`.
pub fn parse_capabilities(raw: &str) -> Capabilities {
    let mut caps = Capabilities {
        raw: raw.to_string(),
        ..Default::default()
    };

    for token in raw.split(';') {
        let token = token.trim();
        if token.is_empty() {
            continue;
        }
        let (key, value) = match token.split_once('=') {
            Some((k, v)) => (k.trim(), v.trim()),
            None => (token, ""),
        };
        match key {
            "proto" => caps.proto = parse_num(key, value, &mut caps.parse_errors),
            "mtu" => caps.mtu = parse_num(key, value, &mut caps.parse_errors),
            "maxblock" => caps.max_block = parse_num(key, value, &mut caps.parse_errors),
            "feat" => {
                caps.features = value
                    .split(',')
                    .map(str::trim)
                    .filter(|s| !s.is_empty())
                    .map(String::from)
                    .collect();
            }
            other => {
                caps.unknown
                    .insert(other.to_string(), value.to_string());
            }
        }
    }
    caps
}

fn parse_num<T>(key: &str, value: &str, errs: &mut Vec<(String, String)>)
-> Option<T>
where
    T: std::str::FromStr,
{
    match value.parse::<T>() {
        Ok(v) => Some(v),
        Err(_) => {
            errs.push((key.to_string(), value.to_string()));
            None
        }
    }
}

impl Capabilities {
    /// A safe default MTU used when the device does not report one.
    pub fn mtu_or_default(&self) -> usize {
        self.mtu.unwrap_or(256)
    }
    /// A safe default max-block used when the device does not report one.
    pub fn max_block_or_default(&self) -> usize {
        self.max_block.unwrap_or(4096)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_normal_response() {
        let caps = parse_capabilities("proto=1;mtu=256;maxblock=4096;feat=");
        assert_eq!(caps.proto, Some(1));
        assert_eq!(caps.mtu, Some(256));
        assert_eq!(caps.max_block, Some(4096));
        assert!(caps.features.is_empty());
        assert!(caps.unknown.is_empty());
        assert!(caps.parse_errors.is_empty());
        assert_eq!(caps.raw, "proto=1;mtu=256;maxblock=4096;feat=");
    }

    #[test]
    fn parses_features_list() {
        let caps = parse_capabilities("proto=1;mtu=512;maxblock=8192;feat=ble,wifi,gpio");
        assert_eq!(caps.features, vec!["ble", "wifi", "gpio"]);
    }

    #[test]
    fn tolerates_unsupported_zu_format() {
        // nRF52840 libc prints %zu literally as "zu".
        let caps = parse_capabilities("proto=1;mtu=zu;maxblock=zu;feat=");
        assert_eq!(caps.proto, Some(1));
        assert_eq!(caps.mtu, None);
        assert_eq!(caps.max_block, None);
        assert_eq!(
            caps.parse_errors,
            vec![
                ("mtu".to_string(), "zu".to_string()),
                ("maxblock".to_string(), "zu".to_string()),
            ]
        );
        assert_eq!(caps.mtu_or_default(), 256);
        assert_eq!(caps.max_block_or_default(), 4096);
    }

    #[test]
    fn retains_unknown_keys() {
        let caps = parse_capabilities("proto=2;mtu=128;maxblock=2048;feat=x;custom=42");
        assert_eq!(caps.unknown.get("custom").map(|s| s.as_str()), Some("42"));
    }

    #[test]
    fn missing_keys_use_defaults() {
        let caps = parse_capabilities("proto=1");
        assert_eq!(caps.proto, Some(1));
        assert_eq!(caps.mtu, None);
        assert_eq!(caps.max_block_or_default(), 4096);
    }

    #[test]
    fn empty_input_is_safe() {
        let caps = parse_capabilities("");
        assert!(caps.proto.is_none());
        assert!(caps.unknown.is_empty());
    }

    #[test]
    fn tolerates_whitespace() {
        let caps = parse_capabilities(" proto = 1 ; mtu = 256 ; feat = a , b ");
        assert_eq!(caps.proto, Some(1));
        assert_eq!(caps.mtu, Some(256));
        assert_eq!(caps.features, vec!["a", "b"]);
    }
}
