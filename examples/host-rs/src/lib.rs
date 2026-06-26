//! `iotsploit-host` library: a generic SCPI-over-USBTMC host core for
//! `iotsploit-usb` devices.
//!
//! The crate is intentionally dependency-free so the first milestone builds
//! anywhere a Rust toolchain is available. See `docs/iotsploit-usb-rust-host-plan.md`
//! for the full design.

pub mod block;
pub mod caps;
pub mod headers;
pub mod session;
pub mod transport;
pub mod usbtmc_kernel;

use std::fmt;

/// Unified error type for the host core.
#[derive(Debug)]
pub enum Error {
    /// Low-level I/O failure (open/read/write on the transport).
    Io(std::io::Error),
    /// Malformed IEEE 488.2 definite-length arbitrary block.
    Block(block::BlockError),
    /// A SCPI-level problem (non-UTF-8 text response, unexpected framing, ...).
    Scpi { cmd: String, msg: String },
    /// Device/transport level problem (no device, permission denied, ...).
    Device(String),
    /// A response did not arrive in time.
    Timeout,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(e) => write!(f, "io: {}", e),
            Error::Block(e) => write!(f, "block: {}", e),
            Error::Scpi { cmd, msg } => write!(f, "scpi `{}`: {}", cmd, msg),
            Error::Device(s) => write!(f, "device: {}", s),
            Error::Timeout => write!(f, "timeout waiting for response"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Io(e) => Some(e),
            Error::Block(e) => Some(e),
            _ => None,
        }
    }
}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(e)
    }
}

impl From<block::BlockError> for Error {
    fn from(e: block::BlockError) -> Self {
        Error::Block(e)
    }
}

/// Convenience alias used throughout the crate.
pub type Result<T> = std::result::Result<T, Error>;
