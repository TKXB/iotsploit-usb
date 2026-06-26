//! Transport abstraction.
//!
//! One [`Transport::write_msg`] is one USBTMC device-dependent message (one
//! SCPI command), and one [`Transport::read_msg`] is one response message. The
//! concrete backend ([`crate::usbtmc_kernel::UsbtmcKernel`]) lets the Linux
//! kernel USBTMC driver handle USBTMC framing.

use crate::Result;

pub trait Transport {
    /// Send one device-dependent message. The caller is responsible for any
    /// trailing newline (`\n`) SCPI terminator.
    fn write_msg(&mut self, bytes: &[u8]) -> Result<()>;

    /// Read one complete response message. `max_len` is an upper bound the
    /// transport should enforce to avoid unbounded allocation.
    fn read_msg(&mut self, max_len: usize) -> Result<Vec<u8>>;
}
