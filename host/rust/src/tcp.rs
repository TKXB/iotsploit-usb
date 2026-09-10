//! Raw SCPI-over-TCP backend (port 5025 by convention).
//!
//! Unlike USBTMC, where the kernel hands over one framed message per `read()`,
//! TCP is a byte stream: one `read()` returns whatever arrived in one segment,
//! which may be half a response or a whole response plus the head of the next.
//! [`MsgReader`] restores message boundaries by parsing the response itself.
//!
//! Responses are self-describing, so the rule is short:
//!
//! ```text
//! first byte is '#'  ->  read ndigits, read the length field, read that many
//!                        payload bytes, then the '\n' terminator
//! otherwise          ->  read until '\n'
//! ```
//!
//! The terminator is a bare `\n` because
//! `third_party/libscpi/inc/scpi/scpi_user_config.h` sets `SCPI_LINE_ENDING` to
//! `LINE_ENDING_LF`. If that ever becomes CRLF, this module must change too.

use std::io::{ErrorKind, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

use crate::{transport::Transport, Error, Result};

/// Default port for raw SCPI over TCP.
pub const DEFAULT_PORT: u16 = 5025;

/// Splits a SCPI byte stream into whole response messages.
///
/// Generic over [`Read`] so the framing logic can be tested without a socket.
struct MsgReader<R: Read> {
    inner: R,
    /// Bytes read from `inner` but not yet returned to the caller.
    buf: Vec<u8>,
}

/// How many bytes at the front of `buf` form one complete message, if any.
enum Frame {
    /// A complete message occupies this many leading bytes.
    Complete(usize),
    /// Not enough bytes yet; read more and try again.
    More,
}

impl<R: Read> MsgReader<R> {
    fn new(inner: R) -> Self {
        Self {
            inner,
            buf: Vec::new(),
        }
    }

    /// Read until one complete message is available, return it, and keep any
    /// trailing bytes buffered for the next call.
    fn read_msg(&mut self, max_len: usize) -> Result<Vec<u8>> {
        loop {
            match frame_len(&self.buf, max_len)? {
                Frame::Complete(n) => {
                    let msg: Vec<u8> = self.buf.drain(..n).collect();
                    return Ok(msg);
                }
                Frame::More => {
                    // A text response with no terminator in sight must not be
                    // allowed to grow the buffer without bound.
                    if self.buf.len() > max_len {
                        return Err(Error::Scpi {
                            cmd: String::new(),
                            msg: format!(
                                "response exceeded {max_len} bytes with no terminator"
                            ),
                        });
                    }
                    let mut chunk = [0u8; 4096];
                    let n = self.inner.read(&mut chunk).map_err(map_read_err)?;
                    if n == 0 {
                        return Err(Error::Device(
                            "connection closed by device".into(),
                        ));
                    }
                    self.buf.extend_from_slice(&chunk[..n]);
                }
            }
        }
    }
}

/// Decide whether `buf` starts with a complete message.
///
/// Returns [`Frame::More`] whenever the buffer is merely short, and an error
/// only when the stream is genuinely malformed.
fn frame_len(buf: &[u8], max_len: usize) -> Result<Frame> {
    if buf.is_empty() {
        return Ok(Frame::More);
    }

    if buf[0] != b'#' {
        return Ok(match buf.iter().position(|&b| b == b'\n') {
            Some(i) => Frame::Complete(i + 1),
            None => Frame::More,
        });
    }

    // #<ndigits><length><payload>\n
    if buf.len() < 2 {
        return Ok(Frame::More);
    }
    let ndigits = match buf[1] {
        d @ b'1'..=b'9' => (d - b'0') as usize,
        // `#0` is indefinite-length; libscpi's SCPI_ResultArbitraryBlock never
        // emits it, so seeing one means the stream is not what we think it is.
        other => {
            return Err(scpi_err(format!(
                "invalid block digit count: {:?}",
                other as char
            )))
        }
    };
    if buf.len() < 2 + ndigits {
        return Ok(Frame::More);
    }

    let mut declared: usize = 0;
    for &d in &buf[2..2 + ndigits] {
        if !d.is_ascii_digit() {
            return Err(scpi_err("non-digit byte in block length field".into()));
        }
        declared = declared
            .saturating_mul(10)
            .saturating_add((d - b'0') as usize);
    }
    if declared > max_len {
        return Err(scpi_err(format!(
            "block declares {declared} bytes, over the {max_len} byte limit"
        )));
    }

    let end = 2 + ndigits + declared;
    // +1 for the '\n' libscpi appends after the block.
    if buf.len() < end + 1 {
        return Ok(Frame::More);
    }
    if buf[end] != b'\n' {
        return Err(scpi_err(format!(
            "block of {declared} bytes not followed by a terminator (got {:?})",
            buf[end] as char
        )));
    }
    Ok(Frame::Complete(end + 1))
}

fn scpi_err(msg: String) -> Error {
    Error::Scpi {
        cmd: String::new(),
        msg,
    }
}

/// Map a socket read error, turning the two timeout spellings into
/// [`Error::Timeout`] so a wedged network device behaves like a USB device that
/// gets `-110` from the kernel.
fn map_read_err(e: std::io::Error) -> Error {
    match e.kind() {
        ErrorKind::WouldBlock | ErrorKind::TimedOut => Error::Timeout,
        _ => Error::Io(e),
    }
}

/// Raw SCPI over a TCP socket.
pub struct TcpTransport {
    reader: MsgReader<TcpStream>,
    writer: TcpStream,
    peer: String,
}

impl TcpTransport {
    /// Connect to `addr` (`host:port`, or bare `host` for [`DEFAULT_PORT`]).
    ///
    /// `timeout` bounds both the initial connect and every subsequent read.
    pub fn connect(addr: &str, timeout: Duration) -> Result<Self> {
        let target = if addr.contains(':') {
            addr.to_string()
        } else {
            format!("{addr}:{DEFAULT_PORT}")
        };

        let sockaddr = target
            .to_socket_addrs()
            .map_err(|e| Error::Device(format!("cannot resolve {target}: {e}")))?
            .next()
            .ok_or_else(|| Error::Device(format!("no address for {target}")))?;

        let stream = TcpStream::connect_timeout(&sockaddr, timeout)
            .map_err(|e| Error::Device(format!("cannot connect to {target}: {e}")))?;

        // Without this, Nagle delays every small SCPI response by ~40 ms.
        stream.set_nodelay(true)?;
        stream.set_read_timeout(Some(timeout))?;

        let writer = stream.try_clone()?;
        Ok(Self {
            reader: MsgReader::new(stream),
            writer,
            peer: target,
        })
    }

    pub fn peer(&self) -> &str {
        &self.peer
    }
}

impl Transport for TcpTransport {
    fn write_msg(&mut self, bytes: &[u8]) -> Result<()> {
        self.writer.write_all(bytes)?;
        self.writer.flush()?;
        Ok(())
    }

    fn read_msg(&mut self, max_len: usize) -> Result<Vec<u8>> {
        self.reader.read_msg(max_len)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;

    /// A `Read` that hands back one scripted slice per call, so a message can
    /// be split at any byte offset without involving a socket.
    struct Chunks(VecDeque<Vec<u8>>);

    impl Chunks {
        fn new(chunks: &[&[u8]]) -> Self {
            Self(chunks.iter().map(|c| c.to_vec()).collect())
        }
    }

    impl Read for Chunks {
        fn read(&mut self, out: &mut [u8]) -> std::io::Result<usize> {
            match self.0.pop_front() {
                Some(c) => {
                    let n = c.len().min(out.len());
                    out[..n].copy_from_slice(&c[..n]);
                    Ok(n)
                }
                None => Ok(0), // EOF
            }
        }
    }

    fn reader(chunks: &[&[u8]]) -> MsgReader<Chunks> {
        MsgReader::new(Chunks::new(chunks))
    }

    #[test]
    fn text_response_in_one_chunk() {
        let mut r = reader(&[b"IoTSploit,esp32s3,0001,0.1.0\n"]);
        assert_eq!(r.read_msg(8192).unwrap(), b"IoTSploit,esp32s3,0001,0.1.0\n");
    }

    #[test]
    fn text_response_split_mid_line() {
        let mut r = reader(&[b"IoTSploit,esp", b"32s3,0001,0.1.0\n"]);
        assert_eq!(r.read_msg(8192).unwrap(), b"IoTSploit,esp32s3,0001,0.1.0\n");
    }

    #[test]
    fn two_responses_in_one_chunk_are_returned_separately() {
        let mut r = reader(&[b"first\nsecond\n"]);
        assert_eq!(r.read_msg(8192).unwrap(), b"first\n");
        assert_eq!(r.read_msg(8192).unwrap(), b"second\n");
    }

    #[test]
    fn block_split_inside_length_digits() {
        // #14 + "abcd" + \n, split between the '1' and the '4'.
        let mut r = reader(&[b"#1", b"4abcd\n"]);
        assert_eq!(r.read_msg(8192).unwrap(), b"#14abcd\n");
    }

    #[test]
    fn block_split_inside_payload() {
        let mut r = reader(&[b"#14ab", b"cd\n"]);
        assert_eq!(r.read_msg(8192).unwrap(), b"#14abcd\n");
    }

    #[test]
    fn block_payload_may_contain_newline_and_hash() {
        // Payload is exactly 4 bytes: '\n', '#', 'x', '\n'. Length-driven
        // framing must not stop at the embedded newlines.
        let mut r = reader(&[b"#14\n#x\n\n"]);
        assert_eq!(r.read_msg(8192).unwrap(), b"#14\n#x\n\n");
    }

    #[test]
    fn block_followed_by_next_response_in_same_chunk() {
        let mut r = reader(&[b"#14abcd\nOK\n"]);
        assert_eq!(r.read_msg(8192).unwrap(), b"#14abcd\n");
        assert_eq!(r.read_msg(8192).unwrap(), b"OK\n");
    }

    #[test]
    fn block_arriving_one_byte_at_a_time() {
        let msg = b"#14abcd\n";
        let chunks: Vec<&[u8]> = msg.chunks(1).collect();
        let mut r = reader(&chunks);
        assert_eq!(r.read_msg(8192).unwrap(), msg);
    }

    #[test]
    fn declared_length_over_max_is_rejected() {
        let mut r = reader(&[b"#9999999999"]);
        assert!(matches!(r.read_msg(1024), Err(Error::Scpi { .. })));
    }

    #[test]
    fn indefinite_length_block_is_rejected() {
        let mut r = reader(&[b"#0abc\n"]);
        assert!(matches!(r.read_msg(8192), Err(Error::Scpi { .. })));
    }

    #[test]
    fn non_digit_in_length_field_is_rejected() {
        let mut r = reader(&[b"#2x4abcd\n"]);
        assert!(matches!(r.read_msg(8192), Err(Error::Scpi { .. })));
    }

    #[test]
    fn block_without_trailing_terminator_is_rejected() {
        // Declared 4 bytes, then 'X' instead of '\n' - the stream is desynced.
        let mut r = reader(&[b"#14abcdX"]);
        assert!(matches!(r.read_msg(8192), Err(Error::Scpi { .. })));
    }

    #[test]
    fn runaway_text_response_is_rejected() {
        let big = vec![b'A'; 300];
        let mut r = reader(&[&big, &big]);
        assert!(matches!(r.read_msg(256), Err(Error::Scpi { .. })));
    }

    #[test]
    fn eof_mid_message_is_a_device_error() {
        let mut r = reader(&[b"partial"]);
        assert!(matches!(r.read_msg(8192), Err(Error::Device(_))));
    }
}
