//! Linux `/dev/usbtmcN` backend.
//!
//! This matches the behaviour of the existing Python host: the kernel USBTMC
//! driver handles USBTMC framing, and the host just writes SCPI lines and reads
//! responses.
//!
//! A read returns one complete response message in a single `read()` call when
//! the buffer is at least as large as the response (true for all current
//! `iotsploit-usb` devices, whose responses are well under 8 KiB). The default
//! read size is generous and can be raised with [`UsbtmcKernel::with_read_size`].

use std::fs::{read_dir, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

use crate::{transport::Transport, Error, Result};

/// Linux kernel USBTMC character-device backend.
pub struct UsbtmcKernel {
    file: std::fs::File,
    path: PathBuf,
    read_size: usize,
}

impl UsbtmcKernel {
    /// Open an explicit device node (e.g. `/dev/usbtmc0`).
    pub fn open(path: &Path) -> Result<Self> {
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(path)
            .map_err(|e| map_open_err(e, path))?;
        Ok(Self {
            file,
            path: path.to_path_buf(),
            read_size: 8192,
        })
    }

    /// Auto-detect a single `/dev/usbtmc*` node. Fails if there are none or
    /// more than one (in which case the user must pass `--device`).
    pub fn auto_detect() -> Result<Self> {
        let mut nodes: Vec<PathBuf> = read_dir("/dev")?
            .filter_map(|e| e.ok())
            .map(|e| e.path())
            .filter(|p| is_usbtmc_node(p.as_path()))
            .collect();
        nodes.sort();
        match nodes.len() {
            0 => Err(Error::Device(
                "no /dev/usbtmc* device found - is the board enumerated?".into(),
            )),
            1 => Self::open(&nodes[0]),
            n => Err(Error::Device(format!(
                "found {n} usbtmc devices; specify one with --device: {nodes:?}"
            ))),
        }
    }

    /// Configure the read buffer size. Must exceed the largest expected
    /// response (block payload + header + terminator).
    pub fn with_read_size(mut self, n: usize) -> Self {
        self.read_size = n.max(64);
        self
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
}

fn is_usbtmc_node(path: &Path) -> bool {
    path.file_name()
        .map(|f| f.to_string_lossy().starts_with("usbtmc"))
        .unwrap_or(false)
}

fn map_open_err(e: std::io::Error, path: &Path) -> Error {
    use std::io::ErrorKind::*;
    match e.kind() {
        NotFound => Error::Device(format!("{} not found", path.display())),
        PermissionDenied => Error::Device(format!(
            "permission denied opening {} (run with sudo or add a udev rule)",
            path.display()
        )),
        _ => Error::Io(e),
    }
}

impl Transport for UsbtmcKernel {
    fn write_msg(&mut self, bytes: &[u8]) -> Result<()> {
        self.file.write_all(bytes)?;
        // Best-effort fsync so the kernel submits the OUT URB promptly.
        let _ = self.file.sync_data();
        Ok(())
    }

    fn read_msg(&mut self, max_len: usize) -> Result<Vec<u8>> {
        // Read up to read_size bytes, but never allocate more than max_len.
        let cap = std::cmp::max(self.read_size.min(max_len), 64);
        let mut chunk = vec![0u8; cap];
        // One blocking read returns the whole response for current devices.
        let n = self.file.read(&mut chunk)?;
        if n == 0 {
            return Err(Error::Device("unexpected EOF reading from device".into()));
        }
        chunk.truncate(n);
        Ok(chunk)
    }
}
