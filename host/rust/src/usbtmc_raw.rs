//! Raw USBTMC backend using `rusb` (libusb-1.0).
//!
//! Implements the USBTMC bulk-OUT/IN framing directly, without relying on a
//! kernel USBTMC driver. This enables Windows and macOS support, and is also
//! useful on Linux when the kernel driver is unavailable.
//!
//! Built behind the `raw-usb` cargo feature: `cargo build --features raw-usb`.

#![cfg(feature = "raw-usb")]

use crate::{transport::Transport, Error, Result};
use rusb::{DeviceHandle, GlobalContext};
use std::time::Duration;

// USBTMC protocol constants (USB TMC Class Specification rev 1.0).
const MSGID_DEV_DEP_MSG_OUT: u8 = 1; // host → device (command data)
const MSGID_REQUEST_DEV_DEP_MSG_IN: u8 = 2; // host → device (request response)
const MSGID_DEV_DEP_MSG_IN: u8 = 2; // device → host (response)
const USBTMC_HEADER_SIZE: usize = 12;
const EOM_BIT: u8 = 0x01;

// USB interface class/subclass for USBTMC.
const USB_CLASS_APP_SPEC: u8 = 0xFE;
const USB_SUBCLASS_USBTMC: u8 = 0x03;

/// Raw USBTMC transport backed by `rusb`/libusb.
pub struct UsbtmcRaw {
    handle: DeviceHandle<GlobalContext>,
    ep_out: u8,
    ep_in: u8,
    max_packet_out: usize,
    max_packet_in: usize,
    btag: u8,
    timeout: Duration,
}

impl UsbtmcRaw {
    /// Open a device by VID/PID. If multiple devices share the same VID/PID,
    /// the first match is used (use [`Self::open_by_serial`] to disambiguate).
    pub fn open_vid_pid(vid: u16, pid: u16) -> Result<Self> {
        let handle = rusb::open_device_with_vid_pid(vid, pid)
            .ok_or_else(|| Error::Device(format!("no USB device found with VID={vid:04x} PID={pid:04x}")))?;
        Self::from_handle(handle)
    }

    /// Open a device by USB serial number string.
    pub fn open_by_serial(serial: &str) -> Result<Self> {
        let list = rusb::devices().map_err(map_usb_err)?;
        for device in list.iter() {
            let desc = match device.device_descriptor() {
                Ok(d) => d,
                Err(_) => continue,
            };
            let handle = match device.open() {
                Ok(h) => h,
                Err(_) => continue,
            };
            if let Ok(langs) = handle.read_languages(Duration::from_secs(1)) {
                if let Some(&lang) = langs.first() {
                    if let Ok(s) = handle.read_serial_number_string(lang, &desc, Duration::from_secs(1)) {
                        if s == serial {
                            return Self::from_handle(handle);
                        }
                    }
                }
            }
        }
        Err(Error::Device(format!("no USB device with serial `{serial}` found")))
    }

    /// Auto-detect a single USBTMC device by interface class.
    pub fn auto_detect() -> Result<Self> {
        let list = rusb::devices().map_err(map_usb_err)?;
        let mut matches = Vec::new();
        for device in list.iter() {
            let config = match device.active_config_descriptor() {
                Ok(c) => c,
                Err(_) => continue,
            };
            let found = config.interfaces()
                .flat_map(|i| i.descriptors())
                .find(|d: &rusb::InterfaceDescriptor| d.class_code() == USB_CLASS_APP_SPEC
                    && d.sub_class_code() == USB_SUBCLASS_USBTMC);
            if let Some(idesc) = found {
                matches.push((device, idesc.interface_number()));
            }
        }
        match matches.len() {
            0 => Err(Error::Device(
                "no USBTMC interface found on any USB device".into(),
            )),
            1 => {
                let (device, iface_num) = matches.into_iter().next().unwrap();
                let handle = device.open().map_err(map_usb_err)?;
                Self::from_handle_with_iface(handle, iface_num)
            }
            n => Err(Error::Device(format!(
                "found {n} USBTMC devices; specify one with --vid/--pid or --serial"
            ))),
        }
    }

    /// Build from an already-opened device handle, auto-detecting the USBTMC
    /// interface and endpoints.
    fn from_handle(handle: DeviceHandle<GlobalContext>) -> Result<Self> {
        let config = handle
            .device()
            .active_config_descriptor()
            .map_err(map_usb_err)?;
        for iface in config.interfaces() {
            for idesc in iface.descriptors() {
                if idesc.class_code() == USB_CLASS_APP_SPEC
                    && idesc.sub_class_code() == USB_SUBCLASS_USBTMC
                {
                    return Self::from_handle_with_iface(handle, idesc.interface_number());
                }
            }
        }
        Err(Error::Device(
            "device has no USBTMC interface (class 0xFE/0x03)".into(),
        ))
    }

    /// Build from an opened handle and a known interface number.
    fn from_handle_with_iface(
        handle: DeviceHandle<GlobalContext>,
        iface_num: u8,
    ) -> Result<Self> {
        let config = handle
            .device()
            .active_config_descriptor()
            .map_err(map_usb_err)?;

        // Find the interface and its endpoints.
        let mut ep_out = None;
        let mut ep_in = None;
        let mut max_out = 64usize;
        let mut max_in = 64usize;

        for iface in config.interfaces() {
            for idesc in iface.descriptors() {
                if idesc.interface_number() != iface_num {
                    continue;
                }
                for ep in idesc.endpoint_descriptors() {
                    let addr = ep.address();
                    if ep.direction() == rusb::Direction::Out {
                        ep_out = Some(addr);
                        max_out = ep.max_packet_size().max(64) as usize;
                    } else if ep.direction() == rusb::Direction::In && ep.transfer_type() == rusb::TransferType::Bulk {
                        ep_in = Some(addr);
                        max_in = ep.max_packet_size().max(64) as usize;
                    }
                }
            }
        }

        let ep_out = ep_out.ok_or_else(|| Error::Device("USBTMC interface has no bulk-OUT endpoint".into()))?;
        let ep_in = ep_in.ok_or_else(|| Error::Device("USBTMC interface has no bulk-IN endpoint".into()))?;

        // Detach kernel driver if active (Linux: usbtmc kernel driver).
        if handle.kernel_driver_active(iface_num).unwrap_or(false) {
            let _ = handle.detach_kernel_driver(iface_num);
        }
        handle
            .claim_interface(iface_num)
            .map_err(|e| Error::Device(format!("failed to claim USBTMC interface: {e}")))?;

        Ok(Self {
            handle,
            ep_out,
            ep_in,
            max_packet_out: max_out,
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

    /// Build a DEV_DEP_MSG_OUT header + payload, padded to a multiple of
    /// `max_packet_out`.
    fn build_msg_out(&self, data: &[u8], eom: bool) -> Vec<u8> {
        let transfer_size = data.len() as u32;
        let total = USBTMC_HEADER_SIZE + data.len();
        // Pad to multiple of max_packet_out (bulk transfer requirement).
        let padded = total.div_ceil(self.max_packet_out) * self.max_packet_out;
        let mut buf = Vec::with_capacity(padded.max(total));
        buf.push(MSGID_DEV_DEP_MSG_OUT);
        // bTag and bTagInverse are filled in by the caller
        buf.push(0); // placeholder bTag
        buf.push(0); // placeholder ~bTag
        buf.extend_from_slice(&transfer_size.to_le_bytes());
        buf.push(if eom { EOM_BIT } else { 0 });
        buf.extend_from_slice(&[0, 0, 0, 0]); // reserved
        buf.extend_from_slice(data);
        // Zero-pad to max_packet_out boundary
        while buf.len() < padded {
            buf.push(0);
        }
        buf
    }

    /// Send a DEV_DEP_MSG_OUT (host → device command data) with EOM.
    fn send_msg_out(&mut self, data: &[u8]) -> Result<()> {
        let btag = self.next_btag();
        let mut msg = self.build_msg_out(data, true);
        msg[1] = btag;
        msg[2] = !btag;

        self.handle
            .write_bulk(self.ep_out, &msg, self.timeout)
            .map_err(map_usb_err)?;
        Ok(())
    }

    /// Request and read a DEV_DEP_MSG_IN (device → host response).
    ///
    /// Sends REQUEST_DEV_DEP_MSG_IN on the bulk-OUT endpoint, then reads the
    /// response on bulk-IN. A DEV_DEP_MSG_IN response is a single 12-byte
    /// header followed by `TransferSize` payload bytes; only the *first* USB
    /// packet of the response carries the header, the rest are pure payload.
    /// The payload may span several USB packets / `read()` calls, so we parse
    /// the header once and then accumulate exactly `TransferSize` bytes before
    /// checking EOM. If the device could not fit the whole logical message in
    /// one response (EOM clear), we issue another request and append, until EOM
    /// or `max_len` is reached.
    fn read_msg_in(&mut self, max_len: usize) -> Result<Vec<u8>> {
        let mpi = self.max_packet_in.max(64);
        let mut result = Vec::new();

        loop {
            // Ask for as much as the caller still wants in this response.
            let want = max_len.saturating_sub(result.len()).max(1);
            let btag = self.next_btag();
            let transfer_size = (want as u32).min(u32::MAX);

            // Build and send the request header.
            let mut req = vec![0u8; USBTMC_HEADER_SIZE];
            req[0] = MSGID_REQUEST_DEV_DEP_MSG_IN;
            req[1] = btag;
            req[2] = !btag;
            req[3..7].copy_from_slice(&transfer_size.to_le_bytes());
            req[7] = 0; // no transfer attributes
            // req[8..12] already zero
            while req.len() < self.max_packet_out {
                req.push(0);
            }
            self.handle
                .write_bulk(self.ep_out, &req, self.timeout)
                .map_err(map_usb_err)?;

            // First read of this response: carries the header. Size the buffer
            // to hold the whole expected response so a small one usually
            // arrives in a single read (and its trailing short packet ends it).
            let cap = (USBTMC_HEADER_SIZE + want).div_ceil(mpi) * mpi;
            let mut buf = vec![0u8; cap.max(mpi)];
            let n = self
                .handle
                .read_bulk(self.ep_in, &mut buf, self.timeout)
                .map_err(map_usb_err)?;
            if n < USBTMC_HEADER_SIZE {
                return Err(Error::Device(format!(
                    "USBTMC response too short: {n} bytes"
                )));
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

            let payload_size = u32::from_le_bytes([buf[3], buf[4], buf[5], buf[6]]) as usize;
            let eom = (buf[7] & EOM_BIT) != 0;

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
                let mut more = vec![0u8; remaining.div_ceil(mpi) * mpi + mpi];
                let m = self
                    .handle
                    .read_bulk(self.ep_in, &mut more, self.timeout)
                    .map_err(map_usb_err)?;
                if m == 0 {
                    return Err(Error::Device(
                        "USBTMC IN ended before the declared payload was received".into(),
                    ));
                }
                let take = m.min(remaining);
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

impl Drop for UsbtmcRaw {
    fn drop(&mut self) {
        // Release the interface so other drivers (e.g. kernel usbtmc) can
        // reclaim it.
        let iface = self.handle
            .device()
            .active_config_descriptor()
            .ok()
            .and_then(|c| {
                c.interfaces()
                    .flat_map(|i| i.descriptors())
                    .find(|d: &rusb::InterfaceDescriptor| d.class_code() == USB_CLASS_APP_SPEC && d.sub_class_code() == USB_SUBCLASS_USBTMC)
                    .map(|d| d.interface_number())
            });
        if let Some(iface_num) = iface {
            let _ = self.handle.release_interface(iface_num);
        }
    }
}

fn map_usb_err(e: rusb::Error) -> Error {
    use rusb::Error::*;
    match e {
        Access => Error::Device("USB access denied (permissions?)".into()),
        NoDevice => Error::Device("USB device disconnected".into()),
        NotFound => Error::Device("USB device not found".into()),
        Busy => Error::Device("USB device busy (kernel driver attached?)".into()),
        Timeout => Error::Timeout,
        other => Error::Device(format!("USB error: {other}")),
    }
}
