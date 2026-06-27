//! Workflow engine: drives multi-step SCPI sequences from profile metadata.
//!
//! Two patterns are supported:
//! - [`WorkflowType::TriggerPollFetch`]: write trigger → poll done → query
//!   count → fetch each result. Used for Wi-Fi scan and BLE scan.
//! - [`WorkflowType::TriggerPollInteractive`]: write trigger → poll state
//!   until success/failure. Used for BLE connect.

use crate::descriptor::{Profile, WorkflowDesc, WorkflowType};
use crate::session::ScpiSession;
use crate::transport::Transport;
use crate::{Error, Result};
use std::time::{Duration, Instant};

/// Outcome of a `trigger_poll_fetch` workflow.
#[derive(Debug, Clone)]
pub struct FetchResult {
    /// Number of results returned by `count_query`.
    pub count: usize,
    /// One entry per `fetch_query(i)` response.
    pub results: Vec<String>,
}

/// Outcome of a `trigger_poll_interactive` workflow.
#[derive(Debug, Clone)]
pub struct InteractiveResult {
    /// Final state value reported by `state_query`.
    pub final_state: String,
    /// Whether the workflow reached the `success_value`.
    pub success: bool,
}

/// Run a workflow by name from a profile.
///
/// `params` are substituted into the trigger command in order.
/// For `trigger_poll_fetch`, prints each result to stdout (one per line).
pub fn run_workflow<T: Transport>(
    session: &mut ScpiSession<T>,
    profile: &Profile,
    name: &str,
    params: &[String],
) -> Result<()> {
    let wf = profile
        .workflow(name)
        .ok_or_else(|| Error::Device(format!("workflow `{name}` not found in profile")))?;

    match wf.workflow_type {
        WorkflowType::TriggerPollFetch => {
            let result = run_trigger_poll_fetch(session, wf, params)?;
            println!("found {} result(s):", result.count);
            for (i, r) in result.results.iter().enumerate() {
                println!("  [{i:2}] {r}");
            }
        }
        WorkflowType::TriggerPollInteractive => {
            let result = run_trigger_poll_interactive(session, wf, params)?;
            if result.success {
                println!("workflow `{name}` succeeded (state={})", result.final_state);
            } else {
                println!("workflow `{name}` failed (state={})", result.final_state);
            }
        }
    }
    Ok(())
}

/// Execute the `trigger_poll_fetch` pattern.
///
/// 1. `write_and_drain(trigger_cmd + params)`
/// 2. Poll `done_query` until it returns `done_value` (or timeout)
/// 3. Query `count_query` → N
/// 4. For i in 0..N: `query(fetch_query + i)` → collect
pub fn run_trigger_poll_fetch<T: Transport>(
    session: &mut ScpiSession<T>,
    wf: &WorkflowDesc,
    params: &[String],
) -> Result<FetchResult> {
    // 1. Trigger
    let trigger = wf.build_trigger(params);
    session.write_and_drain(&trigger)?;

    // 2. Poll for done
    let done_query = wf
        .done_query
        .as_ref()
        .ok_or_else(|| Error::Device("workflow missing done_query".into()))?;
    let done_value = wf
        .done_value
        .as_deref()
        .ok_or_else(|| Error::Device("workflow missing done_value".into()))?;

    let deadline = Instant::now() + Duration::from_millis(wf.timeout_ms);
    let poll = Duration::from_millis(wf.poll_ms);
    loop {
        let resp = session.query(done_query)?;
        if resp.trim() == done_value {
            break;
        }
        if Instant::now() >= deadline {
            return Err(Error::Timeout);
        }
        std::thread::sleep(poll);
    }

    // 3. Count
    let count_query = wf
        .count_query
        .as_ref()
        .ok_or_else(|| Error::Device("workflow missing count_query".into()))?;
    let count_str = session.query(count_query)?;
    let count: usize = count_str
        .trim()
        .parse()
        .map_err(|_| Error::Scpi {
            cmd: count_query.clone(),
            msg: format!("count query returned non-numeric value: `{count_str}`"),
        })?;

    // 4. Fetch each
    let mut results = Vec::with_capacity(count);
    for i in 0..count {
        let fetch = wf
            .build_fetch(i)
            .ok_or_else(|| Error::Device("workflow missing fetch_query".into()))?;
        let resp = session.query(&fetch)?;
        results.push(resp);
    }

    Ok(FetchResult { count, results })
}

/// Execute the `trigger_poll_interactive` pattern.
///
/// 1. `write_and_drain(trigger_cmd + params)`
/// 2. Poll `state_query` until it returns `success_value` or a `failed_values`
///    entry (or timeout)
pub fn run_trigger_poll_interactive<T: Transport>(
    session: &mut ScpiSession<T>,
    wf: &WorkflowDesc,
    params: &[String],
) -> Result<InteractiveResult> {
    // 1. Trigger
    let trigger = wf.build_trigger(params);
    session.write_and_drain(&trigger)?;

    // 2. Poll for state
    let state_query = wf
        .state_query
        .as_ref()
        .ok_or_else(|| Error::Device("workflow missing state_query".into()))?;
    let success_value = wf
        .success_value
        .as_deref()
        .ok_or_else(|| Error::Device("workflow missing success_value".into()))?;

    let deadline = Instant::now() + Duration::from_millis(wf.timeout_ms);
    let poll = Duration::from_millis(wf.poll_ms);

    loop {
        let resp = session.query(state_query)?;
        let state = resp.trim().to_string();
        if state == success_value {
            return Ok(InteractiveResult {
                final_state: state,
                success: true,
            });
        }
        if wf.failed_values.iter().any(|f| f == &state) {
            return Ok(InteractiveResult {
                final_state: state,
                success: false,
            });
        }
        if Instant::now() >= deadline {
            return Err(Error::Timeout);
        }
        std::thread::sleep(poll);
    }
}

// ── Tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::descriptor;
    use std::cell::RefCell;
    use std::collections::VecDeque;

    struct FakeTransport {
        written: RefCell<Vec<Vec<u8>>>,
        responses: RefCell<VecDeque<Vec<u8>>>,
    }
    impl FakeTransport {
        fn new(responses: &[Vec<u8>]) -> Self {
            Self {
                written: RefCell::new(Vec::new()),
                responses: RefCell::new(responses.iter().cloned().collect()),
            }
        }
    }
    impl Transport for FakeTransport {
        fn write_msg(&mut self, bytes: &[u8]) -> Result<()> {
            self.written.borrow_mut().push(bytes.to_vec());
            Ok(())
        }
        fn read_msg(&mut self, _max: usize) -> Result<Vec<u8>> {
            self.responses
                .borrow_mut()
                .pop_front()
                .ok_or_else(|| Error::Device("no fake response".into()))
        }
    }

    fn session(responses: &[Vec<u8>]) -> ScpiSession<FakeTransport> {
        ScpiSession::new(FakeTransport::new(responses))
    }

    const WIFI_PROFILE: &str = "DEV name=test\nWF wifi-scan type=trigger_poll_fetch trigger=WLAN:SCAN done=WLAN:SCAN:DONE?:1 count=WLAN:SCAN:COUNt? fetch=WLAN:SCAN?#index timeout_ms=5000 poll_ms=10";

    #[test]
    fn trigger_poll_fetch_basic() {
        // Responses: drain(after trigger), done=1, count=2, result0, result1
        let mut s = session(&[
            b"\n".to_vec(),               // drain after WLAN:SCAN
            b"1\n".to_vec(),              // WLAN:SCAN:DONE? -> 1
            b"2\n".to_vec(),              // WLAN:SCAN:COUNt? -> 2
            b"HomeNet,-50,6\n".to_vec(),  // WLAN:SCAN? 0
            b"Guest,-72,11\n".to_vec(),   // WLAN:SCAN? 1
        ]);
        let p = descriptor::parse_str(WIFI_PROFILE).unwrap();
        let wf = p.workflow("wifi-scan").unwrap();
        let result = run_trigger_poll_fetch(&mut s, wf, &[]).unwrap();
        assert_eq!(result.count, 2);
        assert_eq!(result.results[0], "HomeNet,-50,6");
        assert_eq!(result.results[1], "Guest,-72,11");
    }

    #[test]
    fn trigger_poll_fetch_with_params() {
        const BLE_PROFILE: &str = "DEV name=test\nWF ble-scan type=trigger_poll_fetch trigger=BLE:SCAN done=BLE:SCAN:DONE?:1 count=BLE:SCAN:COUNt? fetch=BLE:SCAN?#index timeout_ms=5000 poll_ms=10";
        let mut s = session(&[
            b"\n".to_vec(),          // drain after BLE:SCAN 8
            b"1\n".to_vec(),         // BLE:SCAN:DONE? -> 1
            b"1\n".to_vec(),         // BLE:SCAN:COUNt? -> 1
            b"AA:BB:CC,-67,Dev\n".to_vec(), // BLE:SCAN? 0
        ]);
        let p = descriptor::parse_str(BLE_PROFILE).unwrap();
        let wf = p.workflow("ble-scan").unwrap();
        let result = run_trigger_poll_fetch(&mut s, wf, &["8".into()]).unwrap();
        assert_eq!(result.count, 1);
        assert_eq!(result.results[0], "AA:BB:CC,-67,Dev");

        // Verify the trigger command included the param
        let written = s.transport();
        let first_write = &written.written.borrow()[0];
        assert_eq!(first_write, b"BLE:SCAN 8\n");
    }

    #[test]
    fn trigger_poll_fetch_timeout() {
        // done never returns "1" — always "0". Use a short timeout so the
        // test finishes quickly without running out of fake responses.
        const TIMEOUT_PROFILE: &str = "DEV name=test\nWF wifi-scan type=trigger_poll_fetch trigger=WLAN:SCAN done=WLAN:SCAN:DONE?:1 count=WLAN:SCAN:COUNt? fetch=WLAN:SCAN?#index timeout_ms=50 poll_ms=10";
        let mut s = session(&[
            b"\n".to_vec(),
            b"0\n".to_vec(),
            b"0\n".to_vec(),
            b"0\n".to_vec(),
            b"0\n".to_vec(),
            b"0\n".to_vec(),
            b"0\n".to_vec(),
            b"0\n".to_vec(),
            b"0\n".to_vec(),
            b"0\n".to_vec(),
        ]);
        let p = descriptor::parse_str(TIMEOUT_PROFILE).unwrap();
        let wf = p.workflow("wifi-scan").unwrap();
        let result = run_trigger_poll_fetch(&mut s, wf, &[]);
        assert!(matches!(result, Err(Error::Timeout)));
    }

    #[test]
    fn trigger_poll_fetch_empty_results() {
        let mut s = session(&[
            b"\n".to_vec(),  // drain
            b"1\n".to_vec(), // done
            b"0\n".to_vec(), // count = 0
        ]);
        let p = descriptor::parse_str(WIFI_PROFILE).unwrap();
        let wf = p.workflow("wifi-scan").unwrap();
        let result = run_trigger_poll_fetch(&mut s, wf, &[]).unwrap();
        assert_eq!(result.count, 0);
        assert!(result.results.is_empty());
    }

    #[test]
    fn interactive_success() {
        const CONN_PROFILE: &str = "DEV name=test\nWF ble-connect type=trigger_poll_interactive trigger=BLE:CONNect state=BLE:CONNect:STATe? success=2 failed=3 timeout_ms=5000 poll_ms=10";
        let mut s = session(&[
            b"\n".to_vec(),   // drain after BLE:CONNect 0
            b"1\n".to_vec(),  // connecting
            b"2\n".to_vec(),  // connected
        ]);
        let p = descriptor::parse_str(CONN_PROFILE).unwrap();
        let wf = p.workflow("ble-connect").unwrap();
        let result = run_trigger_poll_interactive(&mut s, wf, &["0".into()]).unwrap();
        assert!(result.success);
        assert_eq!(result.final_state, "2");
    }

    #[test]
    fn interactive_failure() {
        const CONN_PROFILE: &str = "DEV name=test\nWF ble-connect type=trigger_poll_interactive trigger=BLE:CONNect state=BLE:CONNect:STATe? success=2 failed=3 timeout_ms=5000 poll_ms=10";
        let mut s = session(&[
            b"\n".to_vec(),   // drain
            b"1\n".to_vec(),  // connecting
            b"3\n".to_vec(),  // failed
        ]);
        let p = descriptor::parse_str(CONN_PROFILE).unwrap();
        let wf = p.workflow("ble-connect").unwrap();
        let result = run_trigger_poll_interactive(&mut s, wf, &["0".into()]).unwrap();
        assert!(!result.success);
        assert_eq!(result.final_state, "3");
    }

    #[test]
    fn nrf_ble_scan_done_value_zero() {
        // nRF52840: done_value = "0" (idle/finished), not "1"
        const NRF_PROFILE: &str = "DEV name=nrf52840\nWF ble-scan type=trigger_poll_fetch trigger=BLE:SCAN:START done=BLE:SCAN:STATe?:0 count=BLE:SCAN:COUNt? fetch=BLE:SCAN:RESult?#index timeout_ms=5000 poll_ms=10";
        let mut s = session(&[
            b"\n".to_vec(),               // drain after BLE:SCAN:START
            b"1\n".to_vec(),              // still scanning
            b"0\n".to_vec(),              // done (0 = idle)
            b"3\n".to_vec(),              // count = 3
            b"AA:BB:CC:DD:EE:01,-67,S1\n".to_vec(),
            b"AA:BB:CC:DD:EE:02,-81,S2\n".to_vec(),
            b"AA:BB:CC:DD:EE:03,-55,S3\n".to_vec(),
        ]);
        let p = descriptor::parse_str(NRF_PROFILE).unwrap();
        let wf = p.workflow("ble-scan").unwrap();
        let result = run_trigger_poll_fetch(&mut s, wf, &[]).unwrap();
        assert_eq!(result.count, 3);
        assert_eq!(result.results[2], "AA:BB:CC:DD:EE:03,-55,S3");
    }
}
