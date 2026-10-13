//! Raw SCPI-over-TCP backend (port 5025 by convention).
//!
//! TCP is a byte stream; [`crate::stream::MsgReader`] restores the response
//! boundaries.

use std::io::Write;
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

use crate::stream::MsgReader;
use crate::{transport::Transport, Error, Result};

/// Default port for raw SCPI over TCP.
pub const DEFAULT_PORT: u16 = 5025;

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
