//! Raw USBTMC backend using `nusb` (pure-Rust, cross-platform USB).
//!
//! Implements the USBTMC bulk-OUT/IN framing directly, without relying on a
//! kernel USBTMC driver. This enables Windows and macOS support, and is also
//! useful on Linux when the kernel driver is unavailable.
//!
//! Unlike a libusb-backed transport, `nusb` talks to the OS USB stack directly
//! (usbfs / WinUSB / IOKit), so there is no C dependency to build or ship. Its
//! transfers are async; we bridge them to the blocking [`Transport`] trait with
//! `futures_lite::block_on`, racing each transfer against an `async-io` timer to
//! preserve a configurable timeout.
//!
//! Built behind the `raw-usb` cargo feature: `cargo build --features raw-usb`.

#![cfg(feature = "raw-usb")]

use std::future::Future;
use std::time::Duration;

use async_io::Timer;
use futures_lite::FutureExt;
use nusb::transfer::{Direction, EndpointType, RequestBuffer};
use nusb::{Device, Interface};

use crate::{transport::Transport, Error, Result};

// USBTMC protocol constants (USB TMC Class Specification rev 1.0).
const MSGID_DEV_DEP_MSG_OUT: u8 = 1; // host → device (command data)
const MSGID_REQUEST_DEV_DEP_MSG_IN: u8 = 2; // host → device (request response)
const MSGID_DEV_DEP_MSG_IN: u8 = 2; // device → host (response)
const USBTMC_HEADER_SIZE: usize = 12;
const EOM_BIT: u8 = 0x01;

// USB interface class/subclass for USBTMC.
const USB_CLASS_APP_SPEC: u8 = 0xFE;
const USB_SUBCLASS_USBTMC: u8 = 0x03;

/// Raw USBTMC transport backed by `nusb`.
pub struct UsbtmcRaw {
    // The claimed interface keeps the underlying device alive; dropping it
    // releases the interface (no manual Drop impl needed).
    interface: Interface,
    ep_out: u8,
    ep_in: u8,
    max_packet_in: usize,
    btag: u8,
    timeout: Duration,
}

impl UsbtmcRaw {
    /// Open a device by VID/PID. If multiple devices share the same VID/PID,
    /// the first match is used (use [`Self::open_by_serial`] to disambiguate).
    pub fn open_vid_pid(vid: u16, pid: u16) -> Result<Self> {
        let info = nusb::list_devices()
            .map_err(map_io_err)?
            .find(|d| d.vendor_id() == vid && d.product_id() == pid)
            .ok_or_else(|| {
                Error::Device(format!("no USB device found with VID={vid:04x} PID={pid:04x}"))
            })?;
        let device = info.open().map_err(map_io_err)?;
        Self::from_device(device)
    }

    /// Open a device by USB serial number string.
    pub fn open_by_serial(serial: &str) -> Result<Self> {
        // `nusb` exposes the serial number from enumeration, so no per-device
        // open + string-descriptor round-trip is needed.
        let info = nusb::list_devices()
            .map_err(map_io_err)?
            .find(|d| d.serial_number() == Some(serial))
            .ok_or_else(|| Error::Device(format!("no USB device with serial `{serial}` found")))?;
        let device = info.open().map_err(map_io_err)?;
        Self::from_device(device)
    }

    /// Open a device by its USB topology address (bus number + device address).
    ///
    /// This is useful when several identical devices have the same VID/PID and
    /// no serial number.
    pub fn open_by_bus_address(bus_number: u8, device_address: u8) -> Result<Self> {
        let info = nusb::list_devices()
            .map_err(map_io_err)?
            .find(|d| d.bus_number() == bus_number && d.device_address() == device_address)
            .ok_or_else(|| {
                Error::Device(format!(
                    "no USB device at bus {bus_number} address {device_address}"
                ))
            })?;
        let device = info.open().map_err(map_io_err)?;
        Self::from_device(device)
    }

    /// Auto-detect a single USBTMC device by interface class.
    pub fn auto_detect() -> Result<Self> {
        let mut matches: Vec<_> = nusb::list_devices()
            .map_err(map_io_err)?
            .filter(|d| {
                d.interfaces()
                    .any(|i| i.class() == USB_CLASS_APP_SPEC && i.subclass() == USB_SUBCLASS_USBTMC)
            })
            .collect();
        match matches.len() {
            0 => Err(Error::Device(
                "no USBTMC interface found on any USB device".into(),
            )),
            1 => {
                let device = matches.remove(0).open().map_err(map_io_err)?;
                Self::from_device(device)
            }
            n => Err(Error::Device(format!(
                "found {n} USBTMC devices; specify one with --vid/--pid or --serial"
            ))),
        }
    }

    /// Locate the USBTMC interface + bulk endpoints on an opened device, claim
    /// the interface, and build the transport.
    fn from_device(device: Device) -> Result<Self> {
        let config = device
            .active_configuration()
            .map_err(|e| Error::Device(format!("no active USB configuration: {e}")))?;

        let mut iface_num = None;
        let mut ep_out = None;
        let mut ep_in = None;
        let mut max_in = 64usize;

        for alt in config.interface_alt_settings() {
            if alt.class() != USB_CLASS_APP_SPEC || alt.subclass() != USB_SUBCLASS_USBTMC {
                continue;
            }
            iface_num = Some(alt.interface_number());
            for ep in alt.endpoints() {
                if ep.transfer_type() != EndpointType::Bulk {
                    continue;
                }
                match ep.direction() {
                    Direction::Out => {
                        ep_out = Some(ep.address());
                    }
                    Direction::In => {
                        ep_in = Some(ep.address());
                        max_in = (ep.max_packet_size() as usize).max(64);
                    }
                }
            }
            break;
        }

        let iface_num = iface_num
            .ok_or_else(|| Error::Device("device has no USBTMC interface (class 0xFE/0x03)".into()))?;
        let ep_out =
            ep_out.ok_or_else(|| Error::Device("USBTMC interface has no bulk-OUT endpoint".into()))?;
        let ep_in =
            ep_in.ok_or_else(|| Error::Device("USBTMC interface has no bulk-IN endpoint".into()))?;

        // On Linux the kernel usbtmc driver may hold the interface; detach it
        // first. `detach_and_claim_interface` is a no-op detach on Windows/macOS
        // (where no such kernel driver exists), so it is the portable choice.
        let interface = device
            .detach_and_claim_interface(iface_num)
            .map_err(|e| Error::Device(format!("failed to claim USBTMC interface: {e}")))?;

        // A previously attached kernel driver (or an aborted prior session) can
        // leave the bulk endpoints with a non-zero data toggle; a fresh raw
        // claim that starts from toggle 0 would then stall forever. Clearing
        // halt on both endpoints resets host+device toggle. Best-effort: on a
        // freshly enumerated device this is a harmless no-op.
        let _ = interface.clear_halt(ep_out);
        let _ = interface.clear_halt(ep_in);

        Ok(Self {
            interface,
            ep_out,
            ep_in,
            max_packet_in: max_in,
            btag: 0,
            timeout: Duration::from_secs(5),
        })
    }

    /// Set the USB transfer timeout.
    pub fn with_timeout(mut self, ms: u64) -> Self {
        self.timeout = Duration::from_millis(ms);
        self
    }

    /// Next bTag counter (wraps 1..=255).
    fn next_btag(&mut self) -> u8 {
        self.btag = self.btag.wrapping_add(1);
        if self.btag == 0 {
            self.btag = 1;
        }
        self.btag
    }

    /// Build a DEV_DEP_MSG_OUT header + payload, zero-padded to a 4-byte
    /// boundary as required by the USBTMC spec (§3.2.1). bTag/bTagInverse are
    /// placeholders filled by the caller.
    ///
    /// Note: padding is to 4 bytes, *not* to `wMaxPacketSize`. Padding to a full
    /// max-packet boundary would make the bulk-OUT exactly fill a packet, and a
    /// device expecting a terminating short packet (e.g. TinyUSB USBTMC) would
    /// then wait forever and never produce a response.
    fn build_msg_out(&self, data: &[u8], eom: bool) -> Vec<u8> {
        let transfer_size = data.len() as u32;
        let total = USBTMC_HEADER_SIZE + data.len();
        let padded = total.div_ceil(4) * 4;
        // Bulk-OUT header (USBTMC §3.2.1): MsgID, bTag, ~bTag, Reserved(0),
        // then the MsgID-specific 8 bytes — TransferSize(u32 LE) at offset 4..8
        // and bmTransferAttributes (bit0 = EOM) at offset 8.
        let mut buf = Vec::with_capacity(padded);
        buf.push(MSGID_DEV_DEP_MSG_OUT); // 0
        buf.push(0); // 1: placeholder bTag
        buf.push(0); // 2: placeholder ~bTag
        buf.push(0); // 3: Reserved
        buf.extend_from_slice(&transfer_size.to_le_bytes()); // 4..8: TransferSize
        buf.push(if eom { EOM_BIT } else { 0 }); // 8: bmTransferAttributes
        buf.extend_from_slice(&[0, 0, 0]); // 9..12: Reserved
        buf.extend_from_slice(data);
        buf.resize(padded, 0); // zero-pad to 4-byte alignment
        buf
    }

    /// Send a DEV_DEP_MSG_OUT (host → device command data) with EOM.
    fn send_msg_out(&mut self, data: &[u8]) -> Result<()> {
        let btag = self.next_btag();
        let mut msg = self.build_msg_out(data, true);
        msg[1] = btag;
        msg[2] = !btag;
        self.bulk_out(msg)
    }

    /// Submit one bulk-OUT transfer and wait (with timeout) for completion.
    fn bulk_out(&self, data: Vec<u8>) -> Result<()> {
        let comp = block_on_timeout(self.interface.bulk_out(self.ep_out, data), self.timeout)?;
        comp.status.map_err(map_transfer_err)?;
        Ok(())
    }

    /// Submit one bulk-IN transfer and wait (with timeout) for completion,
    /// returning the received bytes.
    fn bulk_in(&self, len: usize) -> Result<Vec<u8>> {
        let comp = block_on_timeout(
            self.interface.bulk_in(self.ep_in, RequestBuffer::new(len)),
            self.timeout,
        )?;
        comp.status.map_err(map_transfer_err)?;
        Ok(comp.data)
    }

    /// Request and read a DEV_DEP_MSG_IN (device → host response).
    ///
    /// Sends REQUEST_DEV_DEP_MSG_IN on the bulk-OUT endpoint, then reads the
    /// response on bulk-IN. A DEV_DEP_MSG_IN response is a single 12-byte
    /// header followed by `TransferSize` payload bytes; only the *first* USB
    /// packet of the response carries the header, the rest are pure payload.
    /// The payload may span several USB packets / reads, so we parse the header
    /// once and then accumulate exactly `TransferSize` bytes before checking
    /// EOM. If the device could not fit the whole logical message in one
    /// response (EOM clear), we issue another request and append, until EOM or
    /// `max_len` is reached.
    fn read_msg_in(&mut self, max_len: usize) -> Result<Vec<u8>> {
        let mpi = self.max_packet_in.max(64);
        let mut result = Vec::new();

        loop {
            // Ask for as much as the caller still wants in this response.
            let want = max_len.saturating_sub(result.len()).max(1);
            let btag = self.next_btag();
            let transfer_size = want as u32;

            // REQUEST_DEV_DEP_MSG_IN header (USBTMC §3.3): MsgID, bTag, ~bTag,
            // Reserved(0), TransferSize(u32 LE) at 4..8, bmTransferAttributes at
            // 8, TermChar at 9, Reserved 10..12. req[3] stays 0 (Reserved).
            let mut req = vec![0u8; USBTMC_HEADER_SIZE];
            req[0] = MSGID_REQUEST_DEV_DEP_MSG_IN;
            req[1] = btag;
            req[2] = !btag;
            req[4..8].copy_from_slice(&transfer_size.to_le_bytes());
            // req[8] bmTransferAttributes = 0 (no TermChar), req[9..12] = 0.
            // The 12-byte header is 4-byte aligned and shorter than
            // wMaxPacketSize, so it is already a terminating short packet — no
            // padding to max-packet size (see build_msg_out).
            self.bulk_out(req)?;

            // First read of this response: carries the header. Size the buffer
            // to hold the whole expected response so a small one usually
            // arrives in a single read (and its trailing short packet ends it).
            let cap = (USBTMC_HEADER_SIZE + want).div_ceil(mpi) * mpi;
            let buf = self.bulk_in(cap.max(mpi))?;
            let n = buf.len();
            if n < USBTMC_HEADER_SIZE {
                return Err(Error::Device(format!("USBTMC response too short: {n} bytes")));
            }
            if buf[0] != MSGID_DEV_DEP_MSG_IN {
                return Err(Error::Device(format!(
                    "unexpected USBTMC msg id: {} (expected {})",
                    buf[0], MSGID_DEV_DEP_MSG_IN
                )));
            }
            if buf[1] != btag {
                return Err(Error::Device(format!(
                    "USBTMC bTag mismatch: got {}, expected {}",
                    buf[1], btag
                )));
            }

            // DEV_DEP_MSG_IN header: TransferSize(u32 LE) at 4..8,
            // bmTransferAttributes (bit0 = EOM) at offset 8.
            let payload_size = u32::from_le_bytes([buf[4], buf[5], buf[6], buf[7]]) as usize;
            let eom = (buf[8] & EOM_BIT) != 0;

            // Payload bytes carried by the first read.
            let avail = n - USBTMC_HEADER_SIZE;
            let mut got = avail.min(payload_size);
            result.extend_from_slice(&buf[USBTMC_HEADER_SIZE..USBTMC_HEADER_SIZE + got]);

            // Remaining payload (no header) arrives in subsequent reads. Read
            // with one extra packet of headroom so a trailing zero-length
            // terminating packet is consumed in the same call rather than
            // polluting the next transfer.
            while got < payload_size {
                let remaining = payload_size - got;
                let more = self.bulk_in(remaining.div_ceil(mpi) * mpi + mpi)?;
                if more.is_empty() {
                    return Err(Error::Device(
                        "USBTMC IN ended before the declared payload was received".into(),
                    ));
                }
                let take = more.len().min(remaining);
                result.extend_from_slice(&more[..take]);
                got += take;
            }

            if eom || result.len() >= max_len {
                break;
            }
        }

        Ok(result)
    }
}

impl Transport for UsbtmcRaw {
    fn write_msg(&mut self, bytes: &[u8]) -> Result<()> {
        self.send_msg_out(bytes)
    }

    fn read_msg(&mut self, max_len: usize) -> Result<Vec<u8>> {
        self.read_msg_in(max_len)
    }
}

/// Drive a `nusb` transfer future to completion on the calling thread, racing it
/// against an `async-io` timer. Returns [`Error::Timeout`] if the timer wins
/// (dropping the transfer future cancels the in-flight transfer).
fn block_on_timeout<F, T>(fut: F, timeout: Duration) -> Result<T>
where
    F: Future<Output = T>,
{
    futures_lite::future::block_on(async move {
        let work = async move { Some(fut.await) };
        let timer = async move {
            Timer::after(timeout).await;
            None
        };
        work.or(timer).await
    })
    .ok_or(Error::Timeout)
}

fn map_io_err(e: std::io::Error) -> Error {
    Error::Device(format!("USB error: {e}"))
}

fn map_transfer_err(e: nusb::transfer::TransferError) -> Error {
    Error::Device(format!("USB transfer error: {e}"))
}
