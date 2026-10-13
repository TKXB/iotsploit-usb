//! SCPI over a BLE GATT link, to a board running `glue/usbscpi_nimble.c`.
//!
//! Wire contract, mirrored from `glue/usbscpi_nimble.h`:
//!
//! ```text
//! service  7d7e0001-ce83-4a46-ac8a-2e014163ef3e   advertised
//! RX       7d7e0002-…                             host writes command bytes (with response)
//! TX       7d7e0003-…                             device notifies response bytes
//! ```
//!
//! Both directions are byte streams, like TCP: a write or a notification is a
//! fragment, not a message, so [`crate::stream::MsgReader`] restores response
//! boundaries. Writes are split to the negotiated ATT MTU - 3.
//!
//! The board serves a link only once it is encrypted with an LE Secure
//! Connections bond. btleplug has no pairing call; the operating system pairs
//! when the board asks for security on connect, or when a write is refused for
//! insufficient encryption. [`BleTransport::connect`] therefore returns only
//! once the board accepts a write.
//!
//! btleplug is async. Each transport owns a small tokio runtime and blocks on
//! it, so this type must not be used from inside another async runtime.

use std::io::{self, Read};
use std::sync::mpsc;
use std::time::{Duration, Instant};

use btleplug::api::{
    Central, CentralEvent, Characteristic, Manager as _, Peripheral as _, ScanFilter, WriteType,
};
use btleplug::platform::{Adapter, Manager, Peripheral};
use futures::StreamExt;
use tokio::runtime::Runtime;
use uuid::Uuid;

use crate::stream::MsgReader;
use crate::{transport::Transport, Error, Result};

/// The SCPI service the board advertises.
pub const SERVICE: Uuid = Uuid::from_u128(0x7d7e0001_ce83_4a46_ac8a_2e014163ef3e);
/// Host -> device command bytes.
pub const RX: Uuid = Uuid::from_u128(0x7d7e0002_ce83_4a46_ac8a_2e014163ef3e);
/// Device -> host response bytes.
pub const TX: Uuid = Uuid::from_u128(0x7d7e0003_ce83_4a46_ac8a_2e014163ef3e);

/// How often a scan re-reads the adapter's device list.
const SCAN_POLL: Duration = Duration::from_millis(200);
/// How long to wait between writes the board refused while pairing settles.
const PAIR_RETRY: Duration = Duration::from_millis(250);

/// A board found by [`scan`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BleDevice {
    /// Platform identifier: the MAC address on Linux and Windows, a per-host
    /// UUID on macOS. `ble://<id>` names this board.
    pub id: String,
    /// Advertised name, if any. `ble://<name>` also names the board.
    pub name: Option<String>,
    /// Signal strength of the last advertisement, in dBm.
    pub rssi: Option<i16>,
}

impl BleDevice {
    /// True if `target` names this board, by id or by name, ignoring case.
    pub fn matches(&self, target: &str) -> bool {
        self.id.eq_ignore_ascii_case(target)
            || self
                .name
                .as_deref()
                .is_some_and(|n| n.eq_ignore_ascii_case(target))
    }
}

/// Every board advertising the SCPI service within `timeout`, strongest first.
pub fn scan(timeout: Duration) -> Result<Vec<BleDevice>> {
    let rt = runtime()?;
    rt.block_on(async {
        let adapter = adapter().await?;
        let found = scan_until(&adapter, Instant::now() + timeout, None).await?;
        Ok(found.into_iter().map(|(_, d)| d).collect())
    })
}

/// Raw SCPI over a BLE GATT link.
pub struct BleTransport {
    // Declared first so the link is torn down before the runtime it runs on.
    peripheral: Peripheral,
    rx: Characteristic,
    reader: MsgReader<Notifications>,
    device: BleDevice,
    timeout: Duration,
    rt: Runtime,
}

impl BleTransport {
    /// Find the board `target` names (id or advertised name), connect,
    /// subscribe to its replies and wait until it accepts commands.
    ///
    /// `timeout` bounds each step (scan, connect, pairing) and every
    /// subsequent read.
    pub fn connect(target: &str, timeout: Duration) -> Result<Self> {
        let rt = runtime()?;
        let (peripheral, rx, notes, device) = rt.block_on(open(target, timeout))?;
        Ok(Self {
            peripheral,
            rx,
            reader: MsgReader::new(Notifications::new(notes, timeout)),
            device,
            timeout,
            rt,
        })
    }

    /// The board this transport is connected to.
    pub fn device(&self) -> &BleDevice {
        &self.device
    }

    /// The negotiated ATT MTU (23 until the two sides agree on more).
    pub fn mtu(&self) -> u16 {
        self.peripheral.mtu()
    }
}

impl Transport for BleTransport {
    fn write_msg(&mut self, bytes: &[u8]) -> Result<()> {
        let chunk = write_chunk(self.peripheral.mtu());
        for part in bytes.chunks(chunk) {
            let write = self
                .peripheral
                .write(&self.rx, part, WriteType::WithResponse);
            self.rt
                .block_on(tokio::time::timeout(self.timeout, write))
                .map_err(|_| Error::Timeout)?
                .map_err(ble_err)?;
        }
        Ok(())
    }

    fn read_msg(&mut self, max_len: usize) -> Result<Vec<u8>> {
        self.reader.read_msg(max_len)
    }
}

impl Drop for BleTransport {
    fn drop(&mut self) {
        let _ = self.rt.block_on(self.peripheral.disconnect());
    }
}

/// Largest write the link carries in one ATT request: MTU minus the 3-byte
/// header, and never less than the 20 bytes every link supports.
fn write_chunk(mtu: u16) -> usize {
    (mtu as usize).saturating_sub(3).max(20)
}

fn runtime() -> Result<Runtime> {
    tokio::runtime::Builder::new_multi_thread()
        .worker_threads(1)
        .enable_all()
        .build()
        .map_err(Error::Io)
}

fn ble_err(e: btleplug::Error) -> Error {
    match e {
        btleplug::Error::TimedOut(_) => Error::Timeout,
        btleplug::Error::PermissionDenied => {
            Error::Device("ble: permission denied; allow this app to use Bluetooth".into())
        }
        other => Error::Device(format!("ble: {other}")),
    }
}

async fn adapter() -> Result<Adapter> {
    let unavailable =
        |e: btleplug::Error| Error::Device(format!("ble: Bluetooth is unavailable ({e})"));
    let manager = Manager::new().await.map_err(unavailable)?;
    manager
        .adapters()
        .await
        .map_err(unavailable)?
        .into_iter()
        .next()
        .ok_or_else(|| Error::Device("ble: no Bluetooth adapter found".into()))
}

/// Scan until `deadline`, or until a board matching `target` appears.
async fn scan_until(
    adapter: &Adapter,
    deadline: Instant,
    target: Option<&str>,
) -> Result<Vec<(Peripheral, BleDevice)>> {
    adapter
        .start_scan(ScanFilter {
            services: vec![SERVICE],
        })
        .await
        .map_err(ble_err)?;
    let found = loop {
        let found = advertisers(adapter).await?;
        let hit = target.is_some_and(|t| found.iter().any(|(_, d)| d.matches(t)));
        if hit || Instant::now() >= deadline {
            break found;
        }
        tokio::time::sleep(SCAN_POLL).await;
    };
    let _ = adapter.stop_scan().await;
    Ok(found)
}

/// Peripherals that advertise the SCPI service, strongest first. The scan
/// filter is not enough on its own: BlueZ also keeps devices from earlier
/// scans in the list.
async fn advertisers(adapter: &Adapter) -> Result<Vec<(Peripheral, BleDevice)>> {
    let mut found = Vec::new();
    for p in adapter.peripherals().await.map_err(ble_err)? {
        let props = p.properties().await.map_err(ble_err)?.unwrap_or_default();
        if !props.services.contains(&SERVICE) {
            continue;
        }
        let device = BleDevice {
            id: p.id().to_string(),
            name: props.local_name.or(props.advertisement_name),
            rssi: props.rssi,
        };
        found.push((p, device));
    }
    found.sort_by_key(|(_, d)| std::cmp::Reverse(d.rssi.unwrap_or(i16::MIN)));
    Ok(found)
}

async fn open(
    target: &str,
    timeout: Duration,
) -> Result<(
    Peripheral,
    Characteristic,
    mpsc::Receiver<Vec<u8>>,
    BleDevice,
)> {
    let adapter = adapter().await?;
    let found = scan_until(&adapter, Instant::now() + timeout, Some(target)).await?;
    let (peripheral, device) = found
        .into_iter()
        .find(|(_, d)| d.matches(target))
        .ok_or_else(|| Error::Device(format!("ble: no board named `{target}` is advertising")))?;

    let step = |what: &'static str| move |_| Error::Device(format!("ble: {what} timed out"));
    tokio::time::timeout(timeout, peripheral.connect())
        .await
        .map_err(step("connect"))?
        .map_err(ble_err)?;
    tokio::time::timeout(timeout, peripheral.discover_services())
        .await
        .map_err(step("service discovery"))?
        .map_err(ble_err)?;

    let chars = peripheral.characteristics();
    let find = |uuid: Uuid| {
        chars
            .iter()
            .find(|c| c.service_uuid == SERVICE && c.uuid == uuid)
            .cloned()
            .ok_or_else(|| {
                Error::Device(format!("ble: `{target}` has no SCPI characteristic {uuid}"))
            })
    };
    let rx = find(RX)?;
    let tx = find(TX)?;

    // Forward TX notifications until the link drops; dropping the sender then
    // ends the reader with "connection closed by device".
    let mut notes = peripheral.notifications().await.map_err(ble_err)?;
    let mut events = adapter.events().await.map_err(ble_err)?;
    let id = peripheral.id();
    let (send, recv) = mpsc::channel();
    tokio::spawn(async move {
        loop {
            tokio::select! {
                note = notes.next() => match note {
                    Some(n) if n.uuid == TX => {
                        if send.send(n.value).is_err() {
                            break;
                        }
                    }
                    Some(_) => {}
                    None => break,
                },
                event = events.next() => match event {
                    Some(CentralEvent::DeviceDisconnected(gone)) if gone == id => break,
                    Some(_) => {}
                    None => break,
                },
            }
        }
    });
    peripheral.subscribe(&tx).await.map_err(ble_err)?;

    // An empty line is a no-op to the board, so writing one shows whether it
    // accepts commands yet. Until pairing completes it refuses the write.
    let deadline = Instant::now() + timeout;
    loop {
        match peripheral.write(&rx, b"\n", WriteType::WithResponse).await {
            Ok(()) => break,
            Err(e) if Instant::now() >= deadline => {
                let _ = peripheral.disconnect().await;
                return Err(Error::Device(format!(
                    "ble: `{target}` refused commands ({e}); it accepts them only on an \
                     encrypted, bonded link, so pair it with this computer"
                )));
            }
            Err(_) => tokio::time::sleep(PAIR_RETRY).await,
        }
    }
    Ok((peripheral, rx, recv, device))
}

/// The notification channel as a byte stream for [`MsgReader`].
struct Notifications {
    recv: mpsc::Receiver<Vec<u8>>,
    pending: Vec<u8>,
    pos: usize,
    timeout: Duration,
}

impl Notifications {
    fn new(recv: mpsc::Receiver<Vec<u8>>, timeout: Duration) -> Self {
        Self {
            recv,
            pending: Vec::new(),
            pos: 0,
            timeout,
        }
    }
}

impl Read for Notifications {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        while self.pos == self.pending.len() {
            match self.recv.recv_timeout(self.timeout) {
                Ok(v) => {
                    self.pending = v;
                    self.pos = 0;
                }
                Err(mpsc::RecvTimeoutError::Timeout) => return Err(io::ErrorKind::TimedOut.into()),
                Err(mpsc::RecvTimeoutError::Disconnected) => return Ok(0),
            }
        }
        let n = out.len().min(self.pending.len() - self.pos);
        out[..n].copy_from_slice(&self.pending[self.pos..self.pos + n]);
        self.pos += n;
        Ok(n)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn uuids_match_the_firmware_contract() {
        assert_eq!(SERVICE.to_string(), "7d7e0001-ce83-4a46-ac8a-2e014163ef3e");
        assert_eq!(RX.to_string(), "7d7e0002-ce83-4a46-ac8a-2e014163ef3e");
        assert_eq!(TX.to_string(), "7d7e0003-ce83-4a46-ac8a-2e014163ef3e");
    }

    #[test]
    fn writes_fit_the_negotiated_mtu() {
        assert_eq!(write_chunk(23), 20);
        assert_eq!(write_chunk(247), 244);
        assert_eq!(
            write_chunk(0),
            20,
            "an unknown MTU falls back to the minimum"
        );
    }

    #[test]
    fn a_board_is_named_by_id_or_name() {
        let d = BleDevice {
            id: "E8:06:90:A1:3C:52".into(),
            name: Some("iotsploit-wifi".into()),
            rssi: Some(-52),
        };
        assert!(d.matches("e8:06:90:a1:3c:52"));
        assert!(d.matches("IOTSPLOIT-WIFI"));
        assert!(!d.matches("iotsploit"));
    }

    fn reader(chunks: &[&[u8]]) -> MsgReader<Notifications> {
        let (send, recv) = mpsc::channel();
        for c in chunks {
            send.send(c.to_vec()).unwrap();
        }
        drop(send);
        MsgReader::new(Notifications::new(recv, Duration::from_millis(50)))
    }

    #[test]
    fn replies_are_reassembled_from_notifications() {
        // A 20-byte-MTU link splits a block reply mid-length and mid-payload.
        let mut r = reader(&[
            b"IoTSploit,ESP32S3-",
            b"BLE-WIFI\n#2",
            b"10abcde",
            b"fghij\n",
            b"",
            b"0,\"No error\"\n",
        ]);
        assert_eq!(r.read_msg(8192).unwrap(), b"IoTSploit,ESP32S3-BLE-WIFI\n");
        assert_eq!(r.read_msg(8192).unwrap(), b"#210abcdefghij\n");
        assert_eq!(r.read_msg(8192).unwrap(), b"0,\"No error\"\n");
    }

    #[test]
    fn a_dropped_link_ends_the_stream() {
        let mut r = reader(&[b"partial"]);
        assert!(matches!(r.read_msg(8192), Err(Error::Device(_))));
    }

    #[test]
    fn a_silent_link_times_out() {
        let (_send, recv) = mpsc::channel::<Vec<u8>>();
        let mut r = MsgReader::new(Notifications::new(recv, Duration::from_millis(20)));
        assert!(matches!(r.read_msg(8192), Err(Error::Timeout)));
    }
}
