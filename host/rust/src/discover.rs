//! Device addresses and discovery for `devices` and `-d`.
//!
//! One address syntax names a device on every transport:
//!
//! ```text
//! usbtmc:0  or /dev/usbtmc0      Linux kernel USBTMC node
//! usb:1209:0001[/SERIAL]         raw USB by VID:PID (hex), optionally serial
//! tcp://host[:port]              raw SCPI over TCP (default port 5025)
//! ```
//!
//! [`list`] returns every USB device it can find, kernel nodes first. A device
//! bound to the kernel `usbtmc` driver appears once, as its kernel node: the
//! raw backend would detach that driver just to read `*IDN?`. TCP devices
//! cannot be discovered and are only reachable by naming them.

use std::path::PathBuf;

/// Where a device is and how to open it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Address {
    /// Kernel USBTMC node, e.g. `/dev/usbtmc0`.
    Kernel(PathBuf),
    /// Raw USB by VID/PID and optional serial number.
    Usb { vid: u16, pid: u16, serial: Option<String> },
    /// Raw USB by bus topology. Used for devices found by [`list`], where
    /// identical boards may share VID, PID and serial.
    UsbBus { bus: u8, addr: u8, label: String },
    /// Raw SCPI over TCP, `host:port`.
    Tcp(String),
}

impl std::fmt::Display for Address {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Address::Kernel(p) => {
                let name = p.file_name().map(|n| n.to_string_lossy()).unwrap_or_default();
                match name.strip_prefix("usbtmc") {
                    Some(n) => write!(f, "usbtmc:{n}"),
                    None => write!(f, "{}", p.display()),
                }
            }
            Address::Usb { vid, pid, serial: Some(s) } => write!(f, "usb:{vid:04x}:{pid:04x}/{s}"),
            Address::Usb { vid, pid, serial: None } => write!(f, "usb:{vid:04x}:{pid:04x}"),
            Address::UsbBus { label, .. } => write!(f, "{label}"),
            Address::Tcp(hp) => write!(f, "tcp://{hp}"),
        }
    }
}

/// Parse a `-d` value other than a list number.
pub fn parse(spec: &str) -> Result<Address, String> {
    if let Some(rest) = spec.strip_prefix("tcp://") {
        if rest.is_empty() {
            return Err("expected tcp://<host>[:<port>]".into());
        }
        let hp = if rest.contains(':') { rest.to_string() } else { format!("{rest}:5025") };
        return Ok(Address::Tcp(hp));
    }
    if let Some(n) = spec.strip_prefix("usbtmc:") {
        if n.is_empty() || !n.bytes().all(|b| b.is_ascii_digit()) {
            return Err(format!("expected usbtmc:<number>, got `{spec}`"));
        }
        return Ok(Address::Kernel(PathBuf::from(format!("/dev/usbtmc{n}"))));
    }
    if spec.starts_with('/') {
        return Ok(Address::Kernel(PathBuf::from(spec)));
    }
    // `usb://` is the older spelling.
    if let Some(rest) = spec.strip_prefix("usb://").or_else(|| spec.strip_prefix("usb:")) {
        let (ids, serial) = match rest.split_once('/') {
            Some((ids, s)) if !s.is_empty() => (ids, Some(s.to_string())),
            _ => (rest, None),
        };
        let (v, p) = ids
            .split_once(':')
            .ok_or_else(|| format!("expected usb:<vid>:<pid>[/<serial>], got `{spec}`"))?;
        let vid = parse_hex(v)?;
        let pid = parse_hex(p)?;
        return Ok(Address::Usb { vid, pid, serial });
    }
    Err(format!(
        "unknown device `{spec}`; use a number from `devices`, usbtmc:N, \
         usb:<vid>:<pid>[/<serial>] or tcp://<host>[:<port>]"
    ))
}

/// Parse a hex USB ID, with or without a `0x` prefix.
pub fn parse_hex(s: &str) -> Result<u16, String> {
    u16::from_str_radix(s.trim_start_matches("0x"), 16).map_err(|_| format!("invalid USB ID `{s}` (hex)"))
}

/// Every USB device that can be reached, kernel nodes first, in a stable order
/// so `-d <number>` picks the same device `devices` printed.
pub fn list() -> Vec<Address> {
    let kernel = kernel_nodes();
    let nodes = kernel.iter().map(|(p, _)| Address::Kernel(p.clone()));
    #[cfg(feature = "raw-usb")]
    {
        let taken: Vec<(u8, u8)> = kernel.iter().filter_map(|(_, bus)| *bus).collect();
        let raw = raw_devices().into_iter().filter(|a| match a {
            Address::UsbBus { bus, addr, .. } => !taken.contains(&(*bus, *addr)),
            _ => true,
        });
        return nodes.chain(raw).collect();
    }
    #[cfg(not(feature = "raw-usb"))]
    nodes.collect()
}

/// `/dev/usbtmc*` nodes with their USB bus/address when sysfs knows it.
fn kernel_nodes() -> Vec<(PathBuf, Option<(u8, u8)>)> {
    let mut nodes: Vec<PathBuf> = std::fs::read_dir("/dev")
        .map(|rd| {
            rd.filter_map(|e| e.ok())
                .map(|e| e.path())
                .filter(|p| {
                    p.file_name()
                        .map(|f| f.to_string_lossy().starts_with("usbtmc"))
                        .unwrap_or(false)
                })
                .collect()
        })
        .unwrap_or_default();
    // usbtmc10 sorts after usbtmc9.
    nodes.sort_by_key(|p| {
        let name = p.file_name().map(|f| f.to_string_lossy().into_owned()).unwrap_or_default();
        let n: u32 = name.trim_start_matches("usbtmc").parse().unwrap_or(u32::MAX);
        (n, name)
    });
    nodes.into_iter().map(|p| {
        let bus = sysfs_bus_addr(&p);
        (p, bus)
    }).collect()
}

/// `/sys/class/usbmisc/usbtmcN/device` is the interface; its parent directory
/// is the USB device, which carries `busnum` and `devnum`.
fn sysfs_bus_addr(node: &std::path::Path) -> Option<(u8, u8)> {
    let name = node.file_name()?.to_string_lossy().into_owned();
    let iface = std::fs::canonicalize(format!("/sys/class/usbmisc/{name}/device")).ok()?;
    let dev = iface.parent()?;
    let read = |f: &str| -> Option<u8> { std::fs::read_to_string(dev.join(f)).ok()?.trim().parse().ok() };
    Some((read("busnum")?, read("devnum")?))
}

#[cfg(feature = "raw-usb")]
fn raw_devices() -> Vec<Address> {
    crate::usbtmc_raw::list_usbtmc()
        .into_iter()
        .map(|d| Address::UsbBus {
            bus: d.bus,
            addr: d.addr,
            label: Address::Usb { vid: d.vid, pid: d.pid, serial: d.serial }.to_string(),
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_every_address_form() {
        assert_eq!(parse("usbtmc:1").unwrap(), Address::Kernel("/dev/usbtmc1".into()));
        assert_eq!(parse("/dev/usbtmc0").unwrap(), Address::Kernel("/dev/usbtmc0".into()));
        assert_eq!(parse("tcp://10.0.0.5").unwrap(), Address::Tcp("10.0.0.5:5025".into()));
        assert_eq!(parse("tcp://h:6000").unwrap(), Address::Tcp("h:6000".into()));
        assert_eq!(
            parse("usb:1209:0001/A1B2").unwrap(),
            Address::Usb { vid: 0x1209, pid: 1, serial: Some("A1B2".into()) }
        );
        assert_eq!(parse("usb://303a:4001").unwrap(), Address::Usb { vid: 0x303a, pid: 0x4001, serial: None });
    }

    #[test]
    fn rejects_malformed_addresses() {
        assert!(parse("usbtmc:x").is_err());
        assert!(parse("usb:1209").is_err());
        assert!(parse("tcp://").is_err());
        assert!(parse("serial0").is_err());
    }

    #[test]
    fn displays_in_the_syntax_it_parses() {
        for s in ["usbtmc:3", "usb:1209:0001/SN", "usb:1209:0001", "tcp://h:5025"] {
            assert_eq!(parse(s).unwrap().to_string(), s);
        }
    }
}
