//! Data-plane client: the continuous record stream.
//!
//! SCPI is request/response by construction, so a device with a continuous
//! source pushes records over a second socket instead. This module reads that
//! socket. Control — opening the source, starting and stopping the capture,
//! reading counters — stays on the SCPI session.
//!
//! The client is deliberately **protocol-agnostic**, exactly like the device
//! glue that feeds it. It learns the record layout at runtime from
//! `SYSTem:STReam:FORMat?` rather than hardcoding one, so a new source is a new
//! schema string and needs no change here. Interpreting the payload — ISO-TP
//! reassembly, UDS, a flash command set — is a layer above this one.
//!
//! Framing is trivial compared to the SCPI socket: every record is exactly
//! `stride` bytes, so a partial read is just an incomplete record to finish.
//! The device guarantees alignment across reconnects, so a fresh connection
//! always starts on a record boundary.

use std::io::{ErrorKind, Read};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

use crate::{session::ScpiSession, transport::Transport, Error, Result};

/// Record layout advertised by the device.
///
/// Parsed from `ver=<n>,stride=<n>,fields=<spec>` — the same `fields=` grammar
/// the workflow descriptor already uses, so a host can render columns for a
/// source it has never seen.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StreamFormat {
    pub version: u32,
    pub stride: usize,
    /// `name:type[:unit]` triples, comma separated. Not interpreted here.
    pub fields: String,
}

impl StreamFormat {
    pub fn parse(s: &str) -> Result<Self> {
        let bad = |m: &str| Error::Scpi {
            cmd: "SYSTem:STReam:FORMat?".into(),
            msg: format!("{m}: {s:?}"),
        };
        let (mut version, mut stride, mut fields) = (None, None, None);
        // `fields=` is last and may itself contain commas, so it is split off
        // rather than tokenised with the rest.
        for (i, part) in s.split(',').enumerate() {
            if let Some(rest) = part.strip_prefix("fields=") {
                let tail = s.splitn(i + 1, ',').last().unwrap_or(rest);
                fields = Some(tail.trim_start_matches("fields=").to_string());
                break;
            }
            if let Some(v) = part.strip_prefix("ver=") {
                version = v.trim().parse().ok();
            } else if let Some(v) = part.strip_prefix("stride=") {
                stride = v.trim().parse().ok();
            }
        }
        let version = version.ok_or_else(|| bad("missing or invalid ver="))?;
        let stride: usize = stride.ok_or_else(|| bad("missing or invalid stride="))?;
        if stride == 0 {
            return Err(bad("stride must be non-zero"));
        }
        Ok(StreamFormat {
            version,
            stride,
            fields: fields.unwrap_or_default(),
        })
    }
}

/// Ask the device where its data plane is and what it emits.
///
/// Returns `None` when the device advertises port 0, which is how a build
/// without a data plane says so.
pub fn discover<T: Transport>(s: &mut ScpiSession<T>) -> Result<Option<(u16, StreamFormat)>> {
    let port: u16 = s
        .query("SYSTem:STReam:PORT?")?
        .trim()
        .parse()
        .map_err(|_| Error::Scpi {
            cmd: "SYSTem:STReam:PORT?".into(),
            msg: "not a port number".into(),
        })?;
    if port == 0 {
        return Ok(None);
    }
    let fmt = StreamFormat::parse(s.query("SYSTem:STReam:FORMat?")?.trim())?;
    Ok(Some((port, fmt)))
}

/// A connected data-plane reader.
pub struct DataPlane {
    sock: TcpStream,
    stride: usize,
    buf: Vec<u8>,
}

impl DataPlane {
    /// Connect to the device's data socket.
    ///
    /// `stride` must come from the device's own `SYSTem:STReam:FORMat?` — see
    /// [`discover`]. Guessing it would desynchronise every record.
    pub fn connect<A: ToSocketAddrs>(addr: A, stride: usize, timeout: Duration) -> Result<Self> {
        if stride == 0 {
            return Err(Error::Device("stream stride must be non-zero".into()));
        }
        let sock = TcpStream::connect(addr)?;
        sock.set_read_timeout(Some(timeout))?;
        sock.set_nodelay(true)?;
        Ok(DataPlane {
            sock,
            stride,
            buf: Vec::new(),
        })
    }

    pub fn stride(&self) -> usize {
        self.stride
    }

    /// Read the next whole record, or `None` when the device closed the stream.
    ///
    /// Returns raw bytes. Decoding belongs to the caller, which knows the
    /// schema; this layer only guarantees whole records.
    pub fn next_record(&mut self) -> Result<Option<Vec<u8>>> {
        while self.buf.len() < self.stride {
            let mut chunk = [0u8; 8192];
            match self.sock.read(&mut chunk) {
                Ok(0) => {
                    // A clean close with a partial record buffered means the
                    // device died mid-write; report it rather than silently
                    // returning a short record.
                    if self.buf.is_empty() {
                        return Ok(None);
                    }
                    return Err(Error::Scpi {
                        cmd: "data plane".into(),
                        msg: format!(
                            "stream closed with {} bytes of a {}-byte record",
                            self.buf.len(),
                            self.stride
                        ),
                    });
                }
                Ok(n) => self.buf.extend_from_slice(&chunk[..n]),
                Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                Err(e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::TimedOut => {
                    return Err(Error::Timeout)
                }
                Err(e) => return Err(Error::Io(e)),
            }
        }
        Ok(Some(self.buf.drain(..self.stride).collect()))
    }
}

/// Tracks the device's in-band drop counter so gaps are **reported, not
/// inferred**.
///
/// Every record carries the producer's running total at capture time. A jump
/// between consecutive records is exactly the number of records lost in that
/// interval — there is no need to guess from a sequence hole, and no ambiguity
/// about whether a gap was loss or idleness.
///
/// The counter's offset within a record is schema-dependent, so the caller
/// supplies it rather than this type assuming a layout.
pub struct GapTracker {
    offset: usize,
    last: Option<u64>,
    total: u64,
}

impl GapTracker {
    pub fn new(offset: usize) -> Self {
        GapTracker {
            offset,
            last: None,
            total: 0,
        }
    }

    /// Feed a record; returns how many records were lost immediately before it.
    pub fn observe(&mut self, rec: &[u8]) -> u64 {
        if rec.len() < self.offset + 8 {
            return 0;
        }
        let mut v = [0u8; 8];
        v.copy_from_slice(&rec[self.offset..self.offset + 8]);
        let dropped = u64::from_le_bytes(v);
        let delta = match self.last {
            // Saturating: a device restart resets the counter, and a negative
            // "gap" is meaningless.
            Some(prev) => dropped.saturating_sub(prev),
            None => 0,
        };
        self.last = Some(dropped);
        self.total += delta;
        delta
    }

    /// Records lost since this tracker started watching.
    pub fn total_lost(&self) -> u64 {
        self.total
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_a_format_string() {
        let f = StreamFormat::parse("ver=1,stride=88,fields=ts_us:u64:us,can_id:u32:hex").unwrap();
        assert_eq!(f.version, 1);
        assert_eq!(f.stride, 88);
        // fields= holds commas of its own and must survive intact.
        assert_eq!(f.fields, "ts_us:u64:us,can_id:u32:hex");
    }

    #[test]
    fn rejects_a_zero_stride() {
        assert!(StreamFormat::parse("ver=1,stride=0,fields=x:u8").is_err());
    }

    #[test]
    fn rejects_a_missing_stride() {
        assert!(StreamFormat::parse("ver=1,fields=x:u8").is_err());
    }

    #[test]
    fn gap_tracker_reports_the_delta_not_the_total() {
        let mut g = GapTracker::new(8);
        let rec = |d: u64| {
            let mut r = vec![0u8; 24];
            r[8..16].copy_from_slice(&d.to_le_bytes());
            r
        };
        assert_eq!(g.observe(&rec(5)), 0); // first record establishes a baseline
        assert_eq!(g.observe(&rec(5)), 0);
        assert_eq!(g.observe(&rec(9)), 4); // four lost in this interval
        assert_eq!(g.total_lost(), 4);
        // A device restart must not produce a nonsense negative gap.
        assert_eq!(g.observe(&rec(0)), 0);
    }
}
