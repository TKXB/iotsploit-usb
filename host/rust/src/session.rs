//! High-level SCPI session over an arbitrary [`Transport`].

use crate::{block, caps, transport::Transport, Error, Result};

/// One entry from the SCPI error queue (`SYSTem:ERRor?`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ScpiError {
    pub code: i32,
    pub message: String,
}

/// A SCPI session that wraps a transport and speaks the `iotsploit-usb`
/// text/block protocol.
pub struct ScpiSession<T: Transport> {
    transport: T,
    read_size: usize,
    max_block_len: usize,
}

impl<T: Transport> ScpiSession<T> {
    pub fn new(transport: T) -> Self {
        Self {
            transport,
            read_size: 8192,
            max_block_len: 1 << 20,
        }
    }

    /// Maximum response size the transport will be asked to read.
    pub fn with_read_size(mut self, n: usize) -> Self {
        self.read_size = n.max(64);
        self
    }

    /// Maximum accepted declared block length when parsing block responses.
    pub fn with_max_block_len(mut self, n: usize) -> Self {
        self.max_block_len = n;
        self
    }

    pub fn transport(&self) -> &T {
        &self.transport
    }
    pub fn transport_mut(&mut self) -> &mut T {
        &mut self.transport
    }

    /// Send a non-query SCPI command. A single `\n` terminator is appended.
    ///
    /// This does **not** read a response: like the Python host, the caller
    /// decides when to query. (The device still emits a `\n` terminator for
    /// non-query commands; a subsequent `query`/`query_raw` will receive that
    /// pending terminator first, so prefer issuing queries after writes only
    /// when needed, or call [`Self::write_and_drain`].)
    pub fn write(&mut self, cmd: &str) -> Result<()> {
        let mut msg = Vec::with_capacity(cmd.len() + 1);
        msg.extend_from_slice(cmd.as_bytes());
        msg.push(b'\n');
        self.transport.write_msg(&msg)
    }

    /// Send a non-query command and consume its (typically empty) terminator
    /// response so the transport stays in sync.
    pub fn write_and_drain(&mut self, cmd: &str) -> Result<()> {
        self.write(cmd)?;
        let _ = self.transport.read_msg(self.read_size)?;
        Ok(())
    }

    /// Send a query and return the raw response bytes (no trimming).
    pub fn query_raw(&mut self, cmd: &str) -> Result<Vec<u8>> {
        self.write(cmd)?;
        self.transport.read_msg(self.read_size)
    }

    /// Send a query and return the response as a string with trailing CR/LF
    /// removed. The response is never decoded as lossy UTF-8; invalid UTF-8 is
    /// an error (use [`Self::query_raw`] for arbitrary bytes).
    pub fn query(&mut self, cmd: &str) -> Result<String> {
        let raw = self.query_raw(cmd)?;
        let mut end = raw.len();
        while end > 0 && matches!(raw[end - 1], b'\n' | b'\r') {
            end -= 1;
        }
        String::from_utf8(raw[..end].to_vec()).map_err(|e| Error::Scpi {
            cmd: cmd.to_string(),
            msg: format!("response is not valid UTF-8: {e}"),
        })
    }

    /// Send a query whose response is an IEEE 488.2 definite-length arbitrary
    /// block; return just the payload bytes.
    pub fn query_block(&mut self, cmd: &str) -> Result<Vec<u8>> {
        let raw = self.query_raw(cmd)?;
        let payload = block::parse_block(&raw, self.max_block_len)?;
        Ok(payload.to_vec())
    }

    /// Send a `:DATA:WRITE`-style command carrying an arbitrary binary block.
    ///
    /// `prefix` is the command text (e.g. `DATA:WRITE`); a single space and the
    /// encoded block are appended, followed by the `\n` terminator.
    pub fn write_block(&mut self, prefix: &str, payload: &[u8]) -> Result<()> {
        let len_str = payload.len().to_string();
        let mut msg = Vec::with_capacity(prefix.len() + 2 + len_str.len() + payload.len() + 1);
        msg.extend_from_slice(prefix.as_bytes());
        if !prefix.is_empty() {
            msg.push(b' ');
        }
        msg.extend_from_slice(&block::encode_block(payload));
        msg.push(b'\n');
        self.transport.write_msg(&msg)
    }

    /// Convenience: `*IDN?`
    pub fn idn(&mut self) -> Result<String> {
        self.query("*IDN?")
    }

    /// Convenience: parse `SYSTem:CAPabilities?`.
    pub fn caps(&mut self) -> Result<caps::Capabilities> {
        let raw = self.query("SYSTem:CAPabilities?")?;
        Ok(caps::parse_capabilities(&raw))
    }

    /// Drain the SCPI error queue via `SYSTem:ERRor?` until it reports
    /// "No error" (code 0).
    pub fn drain_errors(&mut self) -> Result<Vec<ScpiError>> {
        let mut errs = Vec::new();
        loop {
            let resp = self.query("SYSTem:ERRor?")?;
            let (code_part, msg_part) = resp.split_once(',').unwrap_or((&resp, ""));
            let code: i32 = code_part.trim().parse().unwrap_or(-1);
            let message = msg_part.trim().trim_matches('"').to_string();
            if code == 0 || message.eq_ignore_ascii_case("no error") {
                break;
            }
            errs.push(ScpiError { code, message });
            if errs.len() >= 64 {
                break;
            }
        }
        Ok(errs)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::io;

    /// A simple in-memory transport: writes are recorded, reads come from a
    /// pre-seeded queue of response buffers.
    struct FakeTransport {
        written: RefCell<Vec<Vec<u8>>>,
        responses: RefCell<std::collections::VecDeque<Vec<u8>>>,
    }

    impl FakeTransport {
        fn new(responses: &[Vec<u8>]) -> Self {
            Self {
                written: RefCell::new(Vec::new()),
                responses: RefCell::new(responses.iter().cloned().collect()),
            }
        }
        fn last_write(&self) -> Vec<u8> {
            self.written.borrow().last().cloned().unwrap_or_default()
        }
    }

    impl Transport for FakeTransport {
        fn write_msg(&mut self, bytes: &[u8]) -> Result<()> {
            self.written.borrow_mut().push(bytes.to_vec());
            Ok(())
        }
        fn read_msg(&mut self, _max_len: usize) -> Result<Vec<u8>> {
            self.responses
                .borrow_mut()
                .pop_front()
                .ok_or_else(|| Error::Io(io::Error::new(io::ErrorKind::UnexpectedEof, "no more fake responses")))
        }
    }

    fn session_with(responses: &[Vec<u8>]) -> ScpiSession<FakeTransport> {
        ScpiSession::new(FakeTransport::new(responses))
    }

    #[test]
    fn query_trims_trailing_newline() {
        let mut s = session_with(&[b"IoTSploit,nRF52840,0001,0.1.0\n".to_vec()]);
        let idn = s.query("*IDN?").unwrap();
        assert_eq!(idn, "IoTSploit,nRF52840,0001,0.1.0");
        assert_eq!(s.transport.last_write(), b"*IDN?\n");
    }

    #[test]
    fn query_block_returns_payload_only() {
        // 4-byte payload "1234" as #141234 + trailing terminator
        let resp = b"#141234\n".to_vec();
        let mut s = session_with(&[resp]);
        let block = s.query_block("DATA:READ? 4").unwrap();
        assert_eq!(block, b"1234");
    }

    #[test]
    fn query_block_handles_empty_block() {
        let resp = b"#10\n".to_vec();
        let mut s = session_with(&[resp]);
        let block = s.query_block("DATA:READ? 64").unwrap();
        assert!(block.is_empty());
    }

    #[test]
    fn caps_parses_via_session() {
        let resp = b"proto=1;mtu=256;maxblock=4096;feat=\n".to_vec();
        let mut s = session_with(&[resp]);
        let caps = s.caps().unwrap();
        assert_eq!(caps.proto, Some(1));
        assert_eq!(caps.mtu, Some(256));
    }

    #[test]
    fn drain_errors_stops_at_no_error() {
        let responses = vec![
            b"-109,\"Missing parameter\"\n".to_vec(),
            b"0,\"No error\"\n".to_vec(),
        ];
        let mut s = session_with(&responses);
        let errs = s.drain_errors().unwrap();
        assert_eq!(errs.len(), 1);
        assert_eq!(errs[0].code, -109);
    }

    #[test]
    fn write_block_constructs_correct_message() {
        let mut s = session_with(&[]);
        s.write_block("DATA:WRITE", &[0x01, 0x02, 0x03]).unwrap();
        // encode_block([1,2,3]) = "#1" + "3" + payload
        assert_eq!(s.transport.last_write(), b"DATA:WRITE #13\x01\x02\x03\n");
    }

    #[test]
    fn write_appends_single_newline() {
        let mut s = session_with(&[]);
        s.write("GPIO:SET 2,1").unwrap();
        assert_eq!(s.transport.last_write(), b"GPIO:SET 2,1\n");
    }
}
