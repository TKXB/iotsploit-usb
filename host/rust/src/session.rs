//! High-level SCPI session over an arbitrary [`Transport`].

use crate::{block, caps, transport::Transport, Error, Result};

/// One entry from the SCPI error queue (`SYSTem:ERRor?`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ScpiError {
    pub code: i32,
    pub message: String,
}

/// What a command produced, as returned by [`ScpiSession::send_checked`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Output {
    /// A non-query command, or a query that failed before producing output.
    None,
    /// Text response with trailing line endings removed. Multi-line responses
    /// (`SYSTem:HELP:HEADers?`) keep their inner newlines.
    Text(String),
    /// Payload of an IEEE 488.2 definite-length block response.
    Block(Vec<u8>),
}

/// Result of [`ScpiSession::send_checked`]: the command's output plus every
/// error the device queued while running it (empty on success).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Checked {
    pub output: Output,
    pub errors: Vec<ScpiError>,
}

/// `<code>,"<message>"` — the shape of a `SYSTem:ERRor?` reply.
fn parse_error_entry(line: &[u8]) -> Option<ScpiError> {
    let text = std::str::from_utf8(line).ok()?.trim_end_matches(['\r', '\n']);
    let (code, msg) = text.split_once(',')?;
    let code: i32 = code.trim().parse().ok()?;
    let msg = msg.trim();
    if msg.len() < 2 || !msg.starts_with('"') || !msg.ends_with('"') {
        return None;
    }
    Some(ScpiError { code, message: msg[1..msg.len() - 1].to_string() })
}

/// Split a response byte stream into its framed items (`#` blocks by length,
/// text by `\n`). Returns `None` while the last item is still incomplete.
fn split_items(buf: &[u8]) -> Option<Vec<&[u8]>> {
    let mut items = Vec::new();
    let mut i = 0;
    while i < buf.len() {
        let rest = &buf[i..];
        let len = if rest[0] == b'#' && rest.len() > 1 && rest[1].is_ascii_digit() && rest[1] != b'0' {
            let nd = (rest[1] - b'0') as usize;
            let digits = rest.get(2..2 + nd)?;
            let n: usize = std::str::from_utf8(digits).ok()?.parse().ok()?;
            let end = 2 + nd + n;
            if rest.len() < end {
                return None;
            }
            // The block's own terminator, if it has arrived.
            if rest.get(end) == Some(&b'\n') { end + 1 } else { end }
        } else {
            rest.iter().position(|&b| b == b'\n')? + 1
        };
        items.push(&rest[..len]);
        i += len;
    }
    Some(items)
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
    /// This does **not** read a response, because a non-query command does not
    /// produce one: libscpi writes its terminator only when the command
    /// actually generated output (`writeNewLine` is guarded by
    /// `!context->first_output`), so nothing at all leaves the device.
    ///
    /// Over USBTMC it *looks* as though a terminator arrives, but that byte is
    /// fabricated by the TinyUSB glue in response to the host's IN request so
    /// the class does not wedge in `STATE_TX_REQUESTED`. It is manufactured by
    /// the read, not queued by the command, and a raw socket has no equivalent.
    /// Do not add a read here: over TCP it would block until the timeout.
    pub fn write(&mut self, cmd: &str) -> Result<()> {
        let mut msg = Vec::with_capacity(cmd.len() + 1);
        msg.extend_from_slice(cmd.as_bytes());
        msg.push(b'\n');
        self.transport.write_msg(&msg)
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

    /// Send a query whose response is several newline-separated records
    /// terminated by a blank line, and return the whole thing.
    ///
    /// `SYSTem:HELP:HEADers?` is the only such command: it writes one line per
    /// registered pattern straight to the write callback, bypassing libscpi's
    /// result machinery, and libscpi then appends its own terminator because
    /// `first_output` is cleared for every successful query
    /// (`third_party/libscpi/src/parser.c:153`). The response therefore ends
    /// with `\n\n`.
    ///
    /// USBTMC hides the multi-record shape by delivering the lot as one framed
    /// message, but a socket cannot tell those inner newlines from message
    /// boundaries, so the blank line is what ends the read. Both transports are
    /// served by the same loop.
    pub fn query_multiline(&mut self, cmd: &str) -> Result<String> {
        self.write(cmd)?;
        let mut out: Vec<u8> = Vec::new();
        // Bound the loop so a device that never sends the blank line fails as a
        // protocol error rather than spinning.
        for _ in 0..4096 {
            let chunk = self.transport.read_msg(self.read_size)?;
            // Terminator seen: either the blank line on its own (one record per
            // message, as on TCP) or trailing `\n\n` (all records in one
            // message, as on USBTMC).
            let done = chunk.is_empty() || chunk == b"\n" || chunk.ends_with(b"\n\n");
            out.extend_from_slice(&chunk);
            if done {
                return String::from_utf8(out).map_err(|e| Error::Scpi {
                    cmd: cmd.to_string(),
                    msg: format!("response is not valid UTF-8: {e}"),
                });
            }
        }
        Err(Error::Scpi {
            cmd: cmd.to_string(),
            msg: "multi-line response did not terminate with a blank line".into(),
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

    /// Run one command and report whether the device accepted it, in a single
    /// round trip.
    ///
    /// The message sent is `*CLS;<cmd>;:SYSTem:ERRor?`. `*CLS` drops errors
    /// left over from earlier sessions so they are not blamed on `cmd`, and the
    /// trailing error query always produces a reply. That reply matters: a
    /// failed query produces no output at all, so without it a TCP read would
    /// wait for the full timeout. Every response arrives in order — as one
    /// message over USBTMC, one per line over TCP — and the error entry is
    /// always last.
    ///
    /// Only the first queued error comes back with the command; any further
    /// ones are drained with separate `SYSTem:ERRor?` queries.
    ///
    /// Not for `SYSTem:ERRor?` itself (its reply would be indistinguishable
    /// from the appended one) or for `DATA:WRITE` blocks.
    pub fn send_checked(&mut self, cmd: &str) -> Result<Checked> {
        self.write(&format!("*CLS;{cmd};:SYSTem:ERRor?"))?;
        let mut buf: Vec<u8> = Vec::new();
        for _ in 0..4096 {
            buf.extend_from_slice(&self.transport.read_msg(self.read_size)?);
            let Some(items) = split_items(&buf) else { continue };
            let Some((last, body)) = items.split_last() else { continue };
            let Some(first_err) = parse_error_entry(last) else { continue };
            let output = Self::output_of(cmd, body, self.max_block_len)?;
            let mut errors = Vec::new();
            if first_err.code != 0 {
                errors.push(first_err);
                errors.extend(self.drain_errors()?);
            }
            return Ok(Checked { output, errors });
        }
        Err(Error::Scpi {
            cmd: cmd.to_string(),
            msg: "no error-queue reply after the command".into(),
        })
    }

    fn output_of(cmd: &str, body: &[&[u8]], max_block_len: usize) -> Result<Output> {
        match body {
            [] => Ok(Output::None),
            [one] if one.first() == Some(&b'#') => {
                Ok(Output::Block(block::parse_block(one, max_block_len)?.to_vec()))
            }
            _ => {
                let joined: Vec<u8> = body.concat();
                let mut end = joined.len();
                while end > 0 && matches!(joined[end - 1], b'\n' | b'\r') {
                    end -= 1;
                }
                String::from_utf8(joined[..end].to_vec())
                    .map(Output::Text)
                    .map_err(|e| Error::Scpi {
                        cmd: cmd.to_string(),
                        msg: format!("response is not valid UTF-8: {e}"),
                    })
            }
        }
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
    fn send_checked_query_over_tcp_framing() {
        let mut s = session_with(&[b"IoTSploit,tcp-demo,0001,0.1.0\n".to_vec(), b"0,\"No error\"\n".to_vec()]);
        let c = s.send_checked("*IDN?").unwrap();
        assert_eq!(c.output, Output::Text("IoTSploit,tcp-demo,0001,0.1.0".into()));
        assert!(c.errors.is_empty());
        assert_eq!(s.transport.last_write(), b"*CLS;*IDN?;:SYSTem:ERRor?\n");
    }

    #[test]
    fn send_checked_query_over_usbtmc_framing() {
        // USBTMC delivers everything one command produced as one message.
        let mut s = session_with(&[b"0\n0,\"No error\"\n".to_vec()]);
        let c = s.send_checked("GPIO:GET? 1").unwrap();
        assert_eq!(c.output, Output::Text("0".into()));
        assert!(c.errors.is_empty());
    }

    #[test]
    fn send_checked_failed_query_reports_error_without_output() {
        let mut s = session_with(&[
            b"-113,\"Undefined header\"\n".to_vec(),
            b"0,\"No error\"\n".to_vec(),
        ]);
        let c = s.send_checked("FOO:BAR?").unwrap();
        assert_eq!(c.output, Output::None);
        assert_eq!(c.errors, vec![ScpiError { code: -113, message: "Undefined header".into() }]);
    }

    #[test]
    fn send_checked_write_collects_every_queued_error() {
        let mut s = session_with(&[
            b"-109,\"Missing parameter\"\n".to_vec(),
            b"-221,\"Settings conflict\"\n".to_vec(),
            b"0,\"No error\"\n".to_vec(),
        ]);
        let c = s.send_checked("GPIO:SET 1").unwrap();
        assert_eq!(c.output, Output::None);
        assert_eq!(c.errors.len(), 2);
        assert_eq!(c.errors[1].code, -221);
    }

    #[test]
    fn send_checked_block_payload_may_look_like_an_error_line() {
        // Payload `0,"x"\n` is 6 bytes; length framing must not end early.
        let mut s = session_with(&[b"#160,\"x\"\n\n0,\"No error\"\n".to_vec()]);
        let c = s.send_checked("DEMO:DATA? 6").unwrap();
        assert_eq!(c.output, Output::Block(b"0,\"x\"\n".to_vec()));
    }

    #[test]
    fn send_checked_keeps_multiline_output() {
        let mut s = session_with(&[b"*IDN?\nGPIO:SET\n\n".to_vec(), b"0,\"No error\"\n".to_vec()]);
        let c = s.send_checked("SYST:HELP:HEAD?").unwrap();
        assert_eq!(c.output, Output::Text("*IDN?\nGPIO:SET".into()));
    }

    #[test]
    fn write_appends_single_newline() {
        let mut s = session_with(&[]);
        s.write("GPIO:SET 2,1").unwrap();
        assert_eq!(s.transport.last_write(), b"GPIO:SET 2,1\n");
    }
}
