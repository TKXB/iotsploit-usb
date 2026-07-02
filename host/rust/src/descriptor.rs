//! Shared line-record parser for device descriptors and local profiles.
//!
//! The "line-record" format is a flat, line-oriented `TAG key=value` text
//! format. The **same** text is used in two places:
//!
//! 1. As an on-device descriptor returned by `SYSTem:HELP:DESCription?` (M7/M8).
//! 2. As a local profile file bundled with the host for firmware that does not
//!    yet implement the descriptor (M5).
//!
//! ## Grammar (v1)
//!
//! - One record per line. Blank lines and lines starting with `#` are ignored.
//! - A record is a leading **TAG** followed by space-separated `key=value`
//!   tokens.
//! - Values containing spaces are double-quoted; `\"` and `\\` are the only
//!   escapes. Unknown keys are ignored (forward-compatible).
//! - Repeatable keys (e.g. `param=`, `failed=`) may appear multiple times.
//!
//! Tags:
//! - `CMD <pattern>` — one command. Keys: `kind`, `summary`,
//!   `param=<name>:<type>:<req|opt>` (repeatable), `returns=<type>`.
//! - `WF <name>` — one workflow. Keys: `type` (`trigger_poll_fetch` |
//!   `trigger_poll_interactive`), `trigger=<cmd>`, `done=<query>:<done_value>`,
//!   `count=<query>`, `fetch=<query>#<index_param>`, `timeout_ms`, `poll_ms`.
//!   Interactive-only: `state=<query>`, `success=<value>`, `failed=<value>`.
//! - `DEV` — optional device header. Keys: `name`, `idn`, `idn_match`,
//!   `proto`, `mtu`, `max_block`.
//!
//! No `serde`, no `serde_json`, no `toml` — the parser is plain `std`.

use crate::block;
use crate::session::ScpiSession;
use crate::transport::Transport;
use crate::{Error, Result};
use std::collections::BTreeMap;
use std::path::Path;

// ── Data model ──────────────────────────────────────────────────────────────

/// A complete device profile / descriptor parsed from line-record text.
#[derive(Debug, Clone, Default)]
pub struct Profile {
    pub device: DeviceInfo,
    pub commands: Vec<CommandDesc>,
    pub workflows: Vec<WorkflowDesc>,
}

/// Device identification and capabilities.
#[derive(Debug, Clone, Default)]
pub struct DeviceInfo {
    /// Short board name, e.g. `esp32s3`, `nrf52840`.
    pub name: String,
    /// Full `*IDN?` string (from descriptor) or a match substring (from profile).
    pub idn: Option<String>,
    /// Substring matched against `*IDN?` to auto-select this profile.
    pub idn_match: Option<String>,
    pub proto: Option<u32>,
    pub mtu: Option<usize>,
    pub max_block: Option<usize>,
}

/// Metadata for a single SCPI command pattern.
#[derive(Debug, Clone, Default)]
pub struct CommandDesc {
    /// SCPI pattern, e.g. `GPIO:SET`, `ADC:READ?`.
    pub pattern: String,
    /// `command` (no response), `query` (returns a value), or `block` (binary).
    pub kind: String,
    /// Human-readable one-liner.
    pub summary: String,
    /// Declared parameters.
    pub params: Vec<ParamDesc>,
    /// Return type, e.g. `u32`, `bool`, `string`, `block`, `none`.
    pub returns: Option<String>,
}

/// A single command parameter.
#[derive(Debug, Clone, Default)]
pub struct ParamDesc {
    pub name: String,
    /// `u32`, `bool`, `string`, `float`, ...
    pub param_type: String,
    pub required: bool,
}

/// A workflow definition.
///
/// Two types are supported:
/// - `trigger_poll_fetch`: write trigger, poll done, query count, fetch each.
/// - `trigger_poll_interactive`: write trigger, poll state, handle prompts.
#[derive(Debug, Clone)]
pub struct WorkflowDesc {
    pub name: String,
    pub workflow_type: WorkflowType,
    pub summary: String,
    // Common
    pub trigger_cmd: String,
    // trigger_poll_fetch
    pub done_query: Option<String>,
    pub done_value: Option<String>,
    pub count_query: Option<String>,
    pub fetch_query: Option<String>,
    pub fetch_index_param: Option<String>,
    // trigger_poll_interactive
    pub state_query: Option<String>,
    pub success_value: Option<String>,
    pub failed_values: Vec<String>,
    /// Interactive prompts fired when `state_query` reaches a prompt's state.
    pub prompts: Vec<PromptDesc>,
    // Timing
    pub timeout_ms: u64,
    pub poll_ms: u64,
}

impl Default for WorkflowDesc {
    fn default() -> Self {
        Self {
            name: String::new(),
            workflow_type: WorkflowType::TriggerPollFetch,
            summary: String::new(),
            trigger_cmd: String::new(),
            done_query: None,
            done_value: None,
            count_query: None,
            fetch_query: None,
            fetch_index_param: None,
            state_query: None,
            success_value: None,
            failed_values: Vec::new(),
            prompts: Vec::new(),
            timeout_ms: 15_000,
            poll_ms: 250,
        }
    }
}

/// How the host should collect the user's response for an interactive prompt.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PromptKind {
    /// Enter a passkey the peer displays.
    Passkey,
    /// Enter a numeric value.
    Number,
    /// Enter free text.
    Text,
    /// Compare a value and accept/reject (`send_cmd 1` / `send_cmd 0`).
    Confirm,
    /// Display a value for the user to act on elsewhere; nothing is sent back.
    Display,
}

/// One interactive prompt in a `trigger_poll_interactive` workflow.
///
/// Parsed from `prompt=<state>|<kind>|<send_cmd>[|<value_query>]`.
#[derive(Debug, Clone)]
pub struct PromptDesc {
    /// `state_query` value at which this prompt fires.
    pub state: String,
    pub kind: PromptKind,
    /// SCPI header the response is sent with. Empty for [`PromptKind::Display`].
    pub send_cmd: String,
    /// Optional query whose value is read and shown to the user.
    pub value_query: Option<String>,
}

/// Workflow type discriminator.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum WorkflowType {
    TriggerPollFetch,
    TriggerPollInteractive,
}

impl Default for WorkflowType {
    fn default() -> Self {
        WorkflowType::TriggerPollFetch
    }
}

// ── Line-record tokenizer ───────────────────────────────────────────────────

/// Split a line into tokens, respecting double-quoted values.
///
/// A token is a maximal run of non-whitespace characters, except that a `"`
/// starts a quoted region that extends across whitespace until the closing `"`.
/// Escapes `\"` and `\\` are honored inside quotes.
fn tokenize(line: &str) -> Vec<String> {
    let bytes = line.as_bytes();
    let mut tokens = Vec::new();
    let mut i = 0;

    while i < bytes.len() {
        // Skip whitespace
        while i < bytes.len() && bytes[i].is_ascii_whitespace() {
            i += 1;
        }
        if i >= bytes.len() {
            break;
        }

        let mut token = String::new();
        let mut in_quote = false;

        while i < bytes.len() {
            let c = bytes[i];
            if !in_quote && c.is_ascii_whitespace() {
                break;
            }
            if c == b'"' {
                in_quote = !in_quote;
                token.push('"');
                i += 1;
            } else if c == b'\\' && in_quote {
                i += 1;
                if i < bytes.len() {
                    match bytes[i] {
                        b'"' => token.push('"'),
                        b'\\' => token.push('\\'),
                        other => {
                            token.push('\\');
                            token.push(other as char);
                        }
                    }
                    i += 1;
                } else {
                    token.push('\\');
                }
            } else {
                token.push(c as char);
                i += 1;
            }
        }
        tokens.push(token);
    }
    tokens
}

/// Parse `key=value` tokens into (key, value) pairs, stripping quotes.
fn parse_key_values(tokens: &[String]) -> Vec<(String, String)> {
    let mut pairs = Vec::new();
    for tok in tokens {
        if let Some(eq_pos) = tok.find('=') {
            let key = tok[..eq_pos].to_string();
            let raw_val = &tok[eq_pos + 1..];
            let value = if raw_val.starts_with('"') && raw_val.ends_with('"') && raw_val.len() >= 2 {
                unquote(raw_val)
            } else {
                raw_val.to_string()
            };
            pairs.push((key, value));
        }
    }
    pairs
}

/// Unquote a double-quoted string, processing `\"` and `\\` escapes.
fn unquote(s: &str) -> String {
    debug_assert!(s.starts_with('"') && s.ends_with('"') && s.len() >= 2);
    let inner = &s[1..s.len() - 1];
    let mut result = String::new();
    let bytes = inner.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'\\' && i + 1 < bytes.len() {
            match bytes[i + 1] {
                b'"' => result.push('"'),
                b'\\' => result.push('\\'),
                other => {
                    result.push('\\');
                    result.push(other as char);
                }
            }
            i += 2;
        } else {
            result.push(bytes[i] as char);
            i += 1;
        }
    }
    result
}

// ── Parser ──────────────────────────────────────────────────────────────────

/// Errors that can occur while parsing or loading a profile/descriptor.
#[derive(Debug)]
pub enum ParseError {
    Io(std::io::Error),
    Parse { line: usize, msg: String },
}

impl std::fmt::Display for ParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ParseError::Io(e) => write!(f, "io: {e}"),
            ParseError::Parse { line, msg } => write!(f, "line {line}: {msg}"),
        }
    }
}

impl std::error::Error for ParseError {}

impl From<std::io::Error> for ParseError {
    fn from(e: std::io::Error) -> Self {
        ParseError::Io(e)
    }
}

/// Parse line-record text into a [`Profile`].
pub fn parse_str(text: &str) -> std::result::Result<Profile, ParseError> {
    let mut profile = Profile::default();

    for (line_num, raw_line) in text.lines().enumerate() {
        let line = raw_line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }

        let tokens = tokenize(line);
        if tokens.is_empty() {
            continue;
        }

        let tag = tokens[0].as_str();
        match tag {
            "DEV" => {
                let kv = parse_key_values(&tokens[1..]);
                parse_dev(&mut profile, &kv, line_num)?;
            }
            "CMD" => {
                if tokens.len() < 2 {
                    return Err(ParseError::Parse {
                        line: line_num + 1,
                        msg: "CMD requires a pattern".into(),
                    });
                }
                let pattern = strip_quotes(&tokens[1]);
                let kv = parse_key_values(&tokens[2..]);
                let cmd = parse_cmd(&pattern, &kv);
                profile.commands.push(cmd);
            }
            "WF" => {
                if tokens.len() < 2 {
                    return Err(ParseError::Parse {
                        line: line_num + 1,
                        msg: "WF requires a name".into(),
                    });
                }
                let name = strip_quotes(&tokens[1]);
                let kv = parse_key_values(&tokens[2..]);
                let wf = parse_wf(&name, &kv);
                profile.workflows.push(wf);
            }
            _ => {
                // Unknown tag — skip (forward-compatible)
            }
        }
    }

    Ok(profile)
}

/// Strip surrounding double quotes if present (no escape processing).
fn strip_quotes(s: &str) -> String {
    if s.starts_with('"') && s.ends_with('"') && s.len() >= 2 {
        unquote(s)
    } else {
        s.to_string()
    }
}

fn parse_dev(profile: &mut Profile, kv: &[(String, String)], line: usize) -> std::result::Result<(), ParseError> {
    for (key, value) in kv {
        match key.as_str() {
            "name" => profile.device.name = value.clone(),
            "idn" => profile.device.idn = Some(value.clone()),
            "idn_match" => profile.device.idn_match = Some(value.clone()),
            "proto" => {
                profile.device.proto = value.parse().ok();
            }
            "mtu" => {
                profile.device.mtu = value.parse().ok();
            }
            "max_block" => {
                profile.device.max_block = value.parse().ok();
            }
            _ => {} // unknown key, skip
        }
    }
    let _ = line;
    Ok(())
}

fn parse_cmd(pattern: &str, kv: &[(String, String)]) -> CommandDesc {
    let mut cmd = CommandDesc {
        pattern: pattern.to_string(),
        ..Default::default()
    };
    for (key, value) in kv {
        match key.as_str() {
            "kind" => cmd.kind = value.clone(),
            "summary" => cmd.summary = value.clone(),
            "param" => {
                // param=<name>:<type>:<req|opt>
                let parts: Vec<&str> = value.splitn(3, ':').collect();
                if parts.len() == 3 {
                    cmd.params.push(ParamDesc {
                        name: parts[0].to_string(),
                        param_type: parts[1].to_string(),
                        required: parts[2] == "req",
                    });
                }
            }
            "returns" => cmd.returns = Some(value.clone()),
            _ => {} // unknown key, skip
        }
    }
    cmd
}

fn parse_wf(name: &str, kv: &[(String, String)]) -> WorkflowDesc {
    let mut wf = WorkflowDesc {
        name: name.to_string(),
        ..Default::default()
    };
    for (key, value) in kv {
        match key.as_str() {
            "type" => {
                wf.workflow_type = match value.as_str() {
                    "trigger_poll_interactive" => WorkflowType::TriggerPollInteractive,
                    _ => WorkflowType::TriggerPollFetch,
                };
            }
            "summary" => wf.summary = value.clone(),
            "trigger" => wf.trigger_cmd = value.clone(),
            "done" => {
                // done=<query>:<done_value> — split on last ':'
                if let Some(pos) = value.rfind(':') {
                    wf.done_query = Some(value[..pos].to_string());
                    wf.done_value = Some(value[pos + 1..].to_string());
                } else {
                    wf.done_query = Some(value.clone());
                }
            }
            "count" => wf.count_query = Some(value.clone()),
            "fetch" => {
                // fetch=<query>#<index_param>
                if let Some(pos) = value.find('#') {
                    wf.fetch_query = Some(value[..pos].to_string());
                    wf.fetch_index_param = Some(value[pos + 1..].to_string());
                } else {
                    wf.fetch_query = Some(value.clone());
                }
            }
            "state" => wf.state_query = Some(value.clone()),
            "success" => wf.success_value = Some(value.clone()),
            "failed" => wf.failed_values.push(value.clone()),
            "prompt" => {
                if let Some(p) = parse_prompt(value) {
                    wf.prompts.push(p);
                }
            }
            "timeout_ms" => wf.timeout_ms = value.parse().unwrap_or(15_000),
            "poll_ms" => wf.poll_ms = value.parse().unwrap_or(250),
            _ => {} // unknown key, skip
        }
    }
    wf
}

/// Parse a `prompt=<state>|<kind>|<send_cmd>[|<value_query>]` value.
///
/// `|` (not `:`) separates the fields because SCPI headers contain `:`.
/// Returns `None` for malformed records or unknown kinds (forward-compatible).
fn parse_prompt(value: &str) -> Option<PromptDesc> {
    let parts: Vec<&str> = value.split('|').collect();
    if parts.len() < 3 {
        return None;
    }
    let kind = match parts[1] {
        "passkey" => PromptKind::Passkey,
        "number" => PromptKind::Number,
        "text" => PromptKind::Text,
        "confirm" => PromptKind::Confirm,
        "display" => PromptKind::Display,
        _ => return None,
    };
    let value_query = parts
        .get(3)
        .filter(|s| !s.is_empty())
        .map(|s| s.to_string());
    Some(PromptDesc {
        state: parts[0].to_string(),
        kind,
        send_cmd: parts[2].to_string(),
        value_query,
    })
}

// ── File loading ────────────────────────────────────────────────────────────

/// Load a profile from a line-record text file on disk.
pub fn load_file(path: &Path) -> std::result::Result<Profile, ParseError> {
    let text = std::fs::read_to_string(path)?;
    parse_str(&text)
}

// ── Validation ──────────────────────────────────────────────────────────────

/// Validate a profile against the set of headers discovered from the device.
///
/// Returns a list of warnings (commands referenced by workflows that the device
/// does not advertise in `SYST:HELP:HEADers?`). This is non-fatal.
pub fn validate_against_headers(
    profile: &Profile,
    device_headers: &[String],
) -> Vec<String> {
    let mut warnings = Vec::new();
    let header_set: std::collections::HashSet<&str> =
        device_headers.iter().map(|s| s.as_str()).collect();

    for wf in &profile.workflows {
        for cmd in workflow_referenced_commands(wf) {
            let base = cmd.split_whitespace().next().unwrap_or(cmd.as_str());
            if !header_set.contains(base) {
                warnings.push(format!(
                    "workflow `{}` references `{}` which is not in device headers",
                    wf.name, base
                ));
            }
        }
    }
    for cmd in &profile.commands {
        if !header_set.contains(cmd.pattern.as_str()) {
            warnings.push(format!(
                "profile command `{}` is not in device headers",
                cmd.pattern
            ));
        }
    }
    warnings
}

fn workflow_referenced_commands(wf: &WorkflowDesc) -> Vec<String> {
    let mut cmds = vec![wf.trigger_cmd.clone()];
    if let Some(q) = &wf.done_query {
        cmds.push(q.clone());
    }
    if let Some(q) = &wf.count_query {
        cmds.push(q.clone());
    }
    if let Some(q) = &wf.fetch_query {
        cmds.push(q.clone());
    }
    if let Some(q) = &wf.state_query {
        cmds.push(q.clone());
    }
    cmds
}

// ── Device descriptor query ─────────────────────────────────────────────────

/// SCPI command used to query the descriptor.
pub const DESC_QUERY: &str = "SYSTem:HELP:DESCription?";

/// Query `SYSTem:HELP:DESCription?` from the device and parse the descriptor.
///
/// Returns `Ok(None)` if the device does not support the command (SCPI error
/// or an empty block). Returns `Ok(Some(_))` on success.
pub fn fetch<T: Transport>(session: &mut ScpiSession<T>) -> Result<Option<Profile>> {
    let raw = session.query_raw(DESC_QUERY)?;

    // If the response is tiny or doesn't start with '#', the device likely
    // doesn't support the descriptor command.
    if raw.is_empty() || raw[0] != b'#' {
        let _ = session.drain_errors();
        return Ok(None);
    }

    let payload = block::parse_block(&raw, 1 << 20)?;
    if payload.is_empty() {
        return Ok(None);
    }

    let text = std::str::from_utf8(payload).map_err(|e| Error::Scpi {
        cmd: DESC_QUERY.to_string(),
        msg: format!("descriptor is not valid UTF-8: {e}"),
    })?;

    let profile = parse_str(text).map_err(|e| Error::Scpi {
        cmd: DESC_QUERY.to_string(),
        msg: format!("descriptor parse error: {e}"),
    })?;

    Ok(Some(profile))
}

// ── Helper methods ──────────────────────────────────────────────────────────

impl Profile {
    /// Find a workflow by name (case-insensitive).
    pub fn workflow(&self, name: &str) -> Option<&WorkflowDesc> {
        self.workflows
            .iter()
            .find(|w| w.name.eq_ignore_ascii_case(name))
    }

    /// Find a command by pattern (case-insensitive).
    pub fn command(&self, pattern: &str) -> Option<&CommandDesc> {
        self.commands
            .iter()
            .find(|c| c.pattern.eq_ignore_ascii_case(pattern))
    }

    /// Build a map of pattern -> summary for quick lookup.
    pub fn command_map(&self) -> BTreeMap<String, &CommandDesc> {
        self.commands
            .iter()
            .map(|c| (c.pattern.clone(), c))
            .collect()
    }
}

impl WorkflowDesc {
    /// Build the trigger command string, substituting user-supplied params.
    ///
    /// Params are appended in order: `trigger_cmd param0 param1 ...`.
    pub fn build_trigger(&self, params: &[String]) -> String {
        if params.is_empty() {
            return self.trigger_cmd.clone();
        }
        let mut cmd = self.trigger_cmd.clone();
        for p in params {
            cmd.push(' ');
            cmd.push_str(p);
        }
        cmd
    }

    /// Build the fetch query for result index `i`.
    pub fn build_fetch(&self, index: usize) -> Option<String> {
        self.fetch_query.as_ref().map(|q| {
            if q.ends_with('?') {
                format!("{q} {index}")
            } else {
                format!("{q} {index}?")
            }
        })
    }
}

// ── Tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::block::encode_block;
    use std::cell::RefCell;
    use std::collections::VecDeque;

    // ── Parser tests ─────────────────────────────────────────────────────────

    const ESP32_TXT: &str = "\
# iotsploit line-record descriptor v1
DEV name=esp32s3 idn_match=\"ESP32\" idn=\"IoTSploit,ESP32S3,0001,0.1.0\" proto=1 mtu=256 max_block=4096
CMD GPIO:SET kind=command summary=\"Set GPIO output level\" param=pin:u32:req param=value:bool:req returns=none
CMD WLAN:SCAN kind=command summary=\"Start Wi-Fi scan\"
CMD WLAN:SCAN:COUNt? kind=query returns=u32
WF wifi-scan type=trigger_poll_fetch trigger=WLAN:SCAN done=WLAN:SCAN:DONE?:1 count=WLAN:SCAN:COUNt? fetch=WLAN:SCAN?#index timeout_ms=15000 poll_ms=250
WF ble-scan type=trigger_poll_fetch trigger=BLE:SCAN done=BLE:SCAN:DONE?:1 count=BLE:SCAN:COUNt? fetch=BLE:SCAN?#index timeout_ms=30000 poll_ms=500
";

    const NRF_TXT: &str = "\
# nRF52840 profile
DEV name=nrf52840 idn_match=\"nRF52840\"
CMD BLE:SCAN:START kind=command summary=\"Start BLE scanning\"
CMD BLE:SCAN:STATe? kind=query summary=\"1 = scanning, 0 = idle\" returns=bool
WF ble-scan type=trigger_poll_fetch trigger=BLE:SCAN:START done=BLE:SCAN:STATe?:0 count=BLE:SCAN:COUNt? fetch=BLE:SCAN:RESult?#index timeout_ms=30000 poll_ms=500
";

    #[test]
    fn parse_esp32_profile() {
        let p = parse_str(ESP32_TXT).unwrap();
        assert_eq!(p.device.name, "esp32s3");
        assert_eq!(p.device.idn_match.as_deref(), Some("ESP32"));
        assert_eq!(p.device.idn.as_deref(), Some("IoTSploit,ESP32S3,0001,0.1.0"));
        assert_eq!(p.device.proto, Some(1));
        assert_eq!(p.device.mtu, Some(256));
        assert_eq!(p.device.max_block, Some(4096));
        assert_eq!(p.commands.len(), 3);
        assert_eq!(p.commands[0].pattern, "GPIO:SET");
        assert_eq!(p.commands[0].kind, "command");
        assert_eq!(p.commands[0].summary, "Set GPIO output level");
        assert_eq!(p.commands[0].params.len(), 2);
        assert_eq!(p.commands[0].params[0].name, "pin");
        assert_eq!(p.commands[0].params[0].param_type, "u32");
        assert!(p.commands[0].params[0].required);
        assert_eq!(p.commands[0].params[1].name, "value");
        assert_eq!(p.commands[0].params[1].param_type, "bool");
        assert!(p.commands[0].params[1].required);
        assert_eq!(p.commands[0].returns.as_deref(), Some("none"));
        assert_eq!(p.workflows.len(), 2);
    }

    #[test]
    fn parse_nrf_profile() {
        let p = parse_str(NRF_TXT).unwrap();
        assert_eq!(p.device.name, "nrf52840");
        assert_eq!(p.commands.len(), 2);
        assert_eq!(p.workflows.len(), 1);
        let wf = &p.workflows[0];
        assert_eq!(wf.workflow_type, WorkflowType::TriggerPollFetch);
        assert_eq!(wf.trigger_cmd, "BLE:SCAN:START");
        assert_eq!(wf.done_query.as_deref(), Some("BLE:SCAN:STATe?"));
        assert_eq!(wf.done_value.as_deref(), Some("0"));
        assert_eq!(wf.fetch_query.as_deref(), Some("BLE:SCAN:RESult?"));
        assert_eq!(wf.fetch_index_param.as_deref(), Some("index"));
    }

    #[test]
    fn parse_interactive_workflow() {
        let txt = "\
WF ble-connect type=trigger_poll_interactive trigger=BLE:CONNect state=BLE:CONNect:STATe? success=2 failed=3 failed=5 timeout_ms=15000 poll_ms=200
";
        let p = parse_str(txt).unwrap();
        assert_eq!(p.workflows.len(), 1);
        let wf = &p.workflows[0];
        assert_eq!(wf.workflow_type, WorkflowType::TriggerPollInteractive);
        assert_eq!(wf.trigger_cmd, "BLE:CONNect");
        assert_eq!(wf.state_query.as_deref(), Some("BLE:CONNect:STATe?"));
        assert_eq!(wf.success_value.as_deref(), Some("2"));
        assert_eq!(wf.failed_values, vec!["3", "5"]);
    }

    #[test]
    fn parse_interactive_workflow_with_prompts() {
        let txt = "\
WF ble-pair type=trigger_poll_interactive trigger=BLE:PAIR state=BLE:PAIR:STATe? success=4 failed=5 prompt=2|passkey|BLE:PAIR:PASSKey prompt=3|confirm|BLE:PAIR:CONFirm|BLE:PAIR:NUMCmp? prompt=6|display||BLE:PAIR:PASSKey? timeout_ms=30000 poll_ms=200
";
        let p = parse_str(txt).unwrap();
        let wf = &p.workflows[0];
        assert_eq!(wf.prompts.len(), 3);

        assert_eq!(wf.prompts[0].state, "2");
        assert_eq!(wf.prompts[0].kind, PromptKind::Passkey);
        assert_eq!(wf.prompts[0].send_cmd, "BLE:PAIR:PASSKey");
        assert_eq!(wf.prompts[0].value_query, None);

        assert_eq!(wf.prompts[1].state, "3");
        assert_eq!(wf.prompts[1].kind, PromptKind::Confirm);
        assert_eq!(wf.prompts[1].send_cmd, "BLE:PAIR:CONFirm");
        assert_eq!(wf.prompts[1].value_query.as_deref(), Some("BLE:PAIR:NUMCmp?"));

        assert_eq!(wf.prompts[2].state, "6");
        assert_eq!(wf.prompts[2].kind, PromptKind::Display);
        assert_eq!(wf.prompts[2].send_cmd, "");
        assert_eq!(wf.prompts[2].value_query.as_deref(), Some("BLE:PAIR:PASSKey?"));
    }

    #[test]
    fn parse_prompt_rejects_malformed_and_unknown() {
        // too few fields, and unknown kind — both skipped, workflow still parses.
        let txt = "WF w type=trigger_poll_interactive trigger=T state=S? success=1 prompt=2|onlytwo prompt=3|bogus|CMD";
        let p = parse_str(txt).unwrap();
        assert!(p.workflows[0].prompts.is_empty());
    }

    #[test]
    fn parse_skips_comments_and_blank_lines() {
        let txt = "\n# comment line\n\nCMD TEST? kind=query\n# another comment";
        let p = parse_str(txt).unwrap();
        assert_eq!(p.commands.len(), 1);
        assert_eq!(p.commands[0].pattern, "TEST?");
    }

    #[test]
    fn parse_unknown_keys_ignored() {
        let txt = "CMD FOO kind=query unknown_key=val another=123";
        let p = parse_str(txt).unwrap();
        assert_eq!(p.commands[0].kind, "query");
    }

    #[test]
    fn parse_quoted_summary_with_spaces() {
        let txt = "CMD FOO kind=command summary=\"This has spaces and \\\"quotes\\\" inside\"";
        let p = parse_str(txt).unwrap();
        assert_eq!(p.commands[0].summary, "This has spaces and \"quotes\" inside");
    }

    #[test]
    fn parse_unknown_tag_skipped() {
        let txt = "UNKNOWN foo=bar\nCMD TEST? kind=query";
        let p = parse_str(txt).unwrap();
        assert_eq!(p.commands.len(), 1);
    }

    #[test]
    fn parse_error_cmd_without_pattern() {
        let txt = "CMD";
        let result = parse_str(txt);
        assert!(matches!(result, Err(ParseError::Parse { .. })));
    }

    #[test]
    fn parse_error_wf_without_name() {
        let txt = "WF";
        let result = parse_str(txt);
        assert!(matches!(result, Err(ParseError::Parse { .. })));
    }

    #[test]
    fn default_timing_values() {
        let txt = "WF test type=trigger_poll_fetch trigger=TRIG done=DONE?:1 count=COUNT? fetch=FETCH?#index";
        let p = parse_str(txt).unwrap();
        let wf = &p.workflows[0];
        assert_eq!(wf.timeout_ms, 15_000);
        assert_eq!(wf.poll_ms, 250);
    }

    // ── Lookup & build tests ─────────────────────────────────────────────────

    #[test]
    fn workflow_lookup_case_insensitive() {
        let p = parse_str(ESP32_TXT).unwrap();
        assert!(p.workflow("WiFi-Scan").is_some());
        assert!(p.workflow("BLE-SCAN").is_some());
        assert!(p.workflow("nonexistent").is_none());
    }

    #[test]
    fn build_trigger_no_params() {
        let p = parse_str(ESP32_TXT).unwrap();
        let wf = p.workflow("wifi-scan").unwrap();
        assert_eq!(wf.build_trigger(&[]), "WLAN:SCAN");
    }

    #[test]
    fn build_trigger_with_params() {
        let p = parse_str(ESP32_TXT).unwrap();
        let wf = p.workflow("ble-scan").unwrap();
        assert_eq!(wf.build_trigger(&["8".into()]), "BLE:SCAN 8");
    }

    #[test]
    fn build_fetch_query() {
        let p = parse_str(ESP32_TXT).unwrap();
        let wf = p.workflow("wifi-scan").unwrap();
        assert_eq!(wf.build_fetch(3).as_deref(), Some("WLAN:SCAN? 3"));
    }

    #[test]
    fn build_fetch_nrf_result() {
        let p = parse_str(NRF_TXT).unwrap();
        let wf = p.workflow("ble-scan").unwrap();
        assert_eq!(wf.build_fetch(2).as_deref(), Some("BLE:SCAN:RESult? 2"));
    }

    // ── Validation tests ─────────────────────────────────────────────────────

    #[test]
    fn validate_flags_missing_commands() {
        let p = parse_str(ESP32_TXT).unwrap();
        let headers = vec![
            "*IDN?".to_string(),
            "WLAN:SCAN".to_string(),
            "WLAN:SCAN:DONE?".to_string(),
            "WLAN:SCAN:COUNt?".to_string(),
            // missing WLAN:SCAN?
            "BLE:SCAN".to_string(),
            "BLE:SCAN:DONE?".to_string(),
            "BLE:SCAN:COUNt?".to_string(),
            "BLE:SCAN?".to_string(),
        ];
        let warnings = validate_against_headers(&p, &headers);
        assert!(warnings.iter().any(|w| w.contains("WLAN:SCAN?")));
        assert!(warnings.iter().any(|w| w.contains("GPIO:SET")));
    }

    #[test]
    fn validate_passes_when_all_present() {
        let p = parse_str(NRF_TXT).unwrap();
        let headers = vec![
            "BLE:SCAN:START".to_string(),
            "BLE:SCAN:STATe?".to_string(),
            "BLE:SCAN:COUNt?".to_string(),
            "BLE:SCAN:RESult?".to_string(),
        ];
        let warnings = validate_against_headers(&p, &headers);
        assert!(warnings.is_empty(), "unexpected warnings: {warnings:?}");
    }

    // ── Device fetch tests ───────────────────────────────────────────────────

    struct FakeTransport {
        responses: RefCell<VecDeque<Vec<u8>>>,
    }
    impl FakeTransport {
        fn new(responses: &[Vec<u8>]) -> Self {
            Self {
                responses: RefCell::new(responses.iter().cloned().collect()),
            }
        }
    }
    impl Transport for FakeTransport {
        fn write_msg(&mut self, _b: &[u8]) -> Result<()> {
            Ok(())
        }
        fn read_msg(&mut self, _m: usize) -> Result<Vec<u8>> {
            self.responses
                .borrow_mut()
                .pop_front()
                .ok_or_else(|| Error::Device("no fake response".into()))
        }
    }

    fn session(responses: &[Vec<u8>]) -> ScpiSession<FakeTransport> {
        ScpiSession::new(FakeTransport::new(responses))
    }

    #[test]
    fn fetch_from_device() {
        let block = encode_block(ESP32_TXT.as_bytes());
        let mut msg = block.clone();
        msg.push(b'\n');
        let mut s = session(&[msg]);
        let desc = fetch(&mut s).unwrap();
        assert!(desc.is_some());
        let desc = desc.unwrap();
        assert_eq!(desc.device.name, "esp32s3");
        assert_eq!(desc.commands.len(), 3);
        assert_eq!(desc.workflows.len(), 2);
    }

    #[test]
    fn fetch_returns_none_when_unsupported() {
        let mut s = session(&[
            b"-113,\" Undefined header\"\n".to_vec(),
            b"0,\"No error\"\n".to_vec(),
        ]);
        let desc = fetch(&mut s).unwrap();
        assert!(desc.is_none());
    }

    #[test]
    fn fetch_returns_none_for_empty_block() {
        let mut s = session(&[b"#10\n".to_vec()]);
        let desc = fetch(&mut s).unwrap();
        assert!(desc.is_none());
    }

    #[test]
    fn tokenize_basic() {
        let tokens = tokenize("CMD GPIO:SET kind=command summary=\"Set GPIO\"");
        assert_eq!(tokens, vec![
            "CMD", "GPIO:SET", "kind=command", "summary=\"Set GPIO\""
        ]);
    }

    #[test]
    fn tokenize_quoted_with_spaces() {
        let tokens = tokenize("DEV idn=\"IoTSploit,ESP32S3,0001,0.1.0\" proto=1");
        assert_eq!(tokens.len(), 3);
        assert_eq!(tokens[0], "DEV");
        assert_eq!(tokens[1], "idn=\"IoTSploit,ESP32S3,0001,0.1.0\"");
        assert_eq!(tokens[2], "proto=1");
    }
}
