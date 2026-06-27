//! Fetch command headers from `SYSTem:HELP:HEADers?`.
//!
//! Current firmware returns a line-separated list of registered command
//! patterns (each on its own line, with a trailing terminator). The full list
//! fits in the device `mtu` for all current `iotsploit-usb` devices, so an
//! un-paged query is tried first. For hypothetical larger command sets we fall
//! back to paged retrieval (`SYST:HELP:HEAD? <offset> <count>`).

use crate::{session::ScpiSession, transport::Transport, Result};

/// Default page size used by the paged fallback. Kept small so a page fits
/// within a 256-byte `mtu` even for ~16-byte command patterns.
pub const DEFAULT_PAGE_SIZE: usize = 8;

/// Fetch all command headers from the device.
pub fn fetch_headers<T: Transport>(
    session: &mut ScpiSession<T>,
    page_size: Option<usize>,
) -> Result<Vec<String>> {
    // 1. Un-paged: works when the whole list fits in mtu (true for all current
    //    devices). An empty result implies the request failed (e.g. too-much-
    //    data on a large command set), so fall through to paging.
    let resp = session.query("SYST:HELP:HEAD?")?;
    let headers = split_headers(&resp);
    if !headers.is_empty() {
        return Ok(headers);
    }
    // 2. Paged fallback.
    fetch_paged(session, page_size.unwrap_or(DEFAULT_PAGE_SIZE))
}

fn fetch_paged<T: Transport>(session: &mut ScpiSession<T>, page_size: usize) -> Result<Vec<String>> {
    let page_size = page_size.max(1);
    let mut all: Vec<String> = Vec::new();
    let mut offset = 0usize;
    for _ in 0..512 {
        let cmd = format!("SYST:HELP:HEAD? {offset} {page_size}");
        let resp = session.query(&cmd)?;
        let page = split_headers(&resp);
        let n = page.len();
        all.extend(page);
        if n < page_size {
            return Ok(all);
        }
        offset = offset.saturating_add(n);
    }
    Ok(all)
}

/// Split a line-separated header response into trimmed, non-empty patterns.
pub fn split_headers(resp: &str) -> Vec<String> {
    resp.split('\n')
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(String::from)
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::collections::VecDeque;

    struct FakeTransport {
        responses: RefCell<VecDeque<Vec<u8>>>,
    }
    impl FakeTransport {
        fn new(responses: &[&[u8]]) -> Self {
            Self {
                responses: RefCell::new(responses.iter().map(|r| r.to_vec()).collect()),
            }
        }
    }
    impl Transport for FakeTransport {
        fn write_msg(&mut self, _b: &[u8]) -> crate::Result<()> {
            Ok(())
        }
        fn read_msg(&mut self, _m: usize) -> crate::Result<Vec<u8>> {
            self.responses
                .borrow_mut()
                .pop_front()
                .ok_or_else(|| crate::Error::Device("no fake response".into()))
        }
    }

    #[test]
    fn split_handles_trailing_blank_lines() {
        // Real device emits an extra trailing newline (libscpi terminator).
        let resp = "*IDN?\n*RST\nBLE:SCAN:CLEar\n\n";
        let h = split_headers(resp);
        assert_eq!(h, vec!["*IDN?", "*RST", "BLE:SCAN:CLEar"]);
    }

    #[test]
    fn split_trims_whitespace() {
        let h = split_headers("  a  \n b \r\n\n");
        assert_eq!(h, vec!["a", "b"]);
    }

    #[test]
    fn fetch_unpaged_short_circuits() {
        let mut s = ScpiSession::new(FakeTransport::new(&[b"*IDN?\n*RST\n*CLS\n\n"]));
        let h = fetch_headers(&mut s, None).unwrap();
        assert_eq!(h, vec!["*IDN?", "*RST", "*CLS"]);
    }

    #[test]
    fn fetch_falls_back_to_paged_when_unpaged_empty() {
        // First (un-paged) response is empty -> paged fallback returns two pages.
        let mut s = ScpiSession::new(FakeTransport::new(&[
            b"\n",                 // un-paged failed/empty
            b"a\nb\n\n",           // page 1 (2 items, < page 3 -> stop)
        ]));
        let h = fetch_headers(&mut s, Some(3)).unwrap();
        assert_eq!(h, vec!["a", "b"]);
    }

    #[test]
    fn fetch_pages_until_short_page() {
        let mut s = ScpiSession::new(FakeTransport::new(&[
            b"\n",          // un-paged empty
            b"a\nb\nc\n\n", // page size 3, full page
            b"d\n\n",       // short page -> stop
        ]));
        let h = fetch_headers(&mut s, Some(3)).unwrap();
        assert_eq!(h, vec!["a", "b", "c", "d"]);
    }
}
