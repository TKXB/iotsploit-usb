//! `iotsploit-host` CLI binary.
//!
//! A command-line tool for talking to `iotsploit-usb` devices over SCPI.
//! Supports multiple transport backends (kernel `/dev/usbtmcN`, raw `nusb`,
//! raw SCPI over TCP),
//! line-record profile metadata, workflow execution, and device descriptors.

use std::io::{self, Write};
use std::path::PathBuf;
use std::process::ExitCode;

use iotsploit_host::{
    caps, headers, session::ScpiSession, transport::Transport, Error,
};

#[cfg(feature = "kernel")]
use iotsploit_host::usbtmc_kernel::UsbtmcKernel;
#[cfg(feature = "raw-usb")]
use iotsploit_host::usbtmc_raw::UsbtmcRaw;
#[cfg(feature = "tcp")]
use iotsploit_host::tcp::TcpTransport;
#[cfg(feature = "tcp")]
use std::time::Duration;

const HELP: &str = "\
iotsploit-host - generic SCPI host for iotsploit-usb (USBTMC and TCP)

USAGE:
    iotsploit-host [options] <command> [args]

GLOBAL OPTIONS:
    --backend auto|kernel|raw|tcp
                                Transport backend (default: auto)
    -d, --device <uri>          Device to open. A scheme selects the backend:
                                  /dev/usbtmc0          kernel USBTMC node
                                  tcp://<host>[:<port>] raw SCPI over TCP (5025)
                                  usb://<vid>:<pid>     raw USB (hex, e.g. 303a:4001)
    --vid <hex> --pid <hex>     USB VID/PID for raw backend (e.g. 1209 0001)
    --serial <string>           USB serial number (raw backend)
    --timeout <ms>              Connect/read timeout for tcp (default: 5000)
    -h, --help                  Show this help
    -V, --version               Show version

COMMANDS:
    list                         List USBTMC device nodes
    idn                          Query *IDN?
    caps                         Query and parse SYSTem:CAPabilities?
    headers                      List command headers (SYSTem:HELP:HEADers?)
    describe                     Query SYSTem:HELP:DESCription? (line-record descriptor)
    query  <cmd>                 Send a SCPI query and print the text response
    write  <cmd>                 Send a SCPI command (no response printed)
    block-read <cmd> [--out F]   Query an arbitrary-block response; write to file
    errors                       Drain the SYSTem:ERRor? queue
    stream [count]               Read records from the device's data plane
    workflow <name> [params...]  Run a workflow defined by the device descriptor
    profile                      Show the device's full command/workflow descriptor
    repl                         Interactive SCPI prompt (Ctrl-D to exit)

Command and workflow metadata is read live from the connected device via
SYSTem:HELP:DESCription? — the device is the single source of truth, so there
are no local profile files to keep in sync.

EXAMPLES:
    iotsploit-host idn
    iotsploit-host caps
    iotsploit-host headers
    iotsploit-host stream 100
    iotsploit-host workflow wifi-scan
    iotsploit-host workflow ble-scan 8
    iotsploit-host --backend raw --vid 1209 --pid 0001 idn
    iotsploit-host --device tcp://192.168.4.1:5025 idn
    iotsploit-host --device tcp://10.42.0.57 workflow wifi-scan
    iotsploit-host describe
    iotsploit-host query '*IDN?'
    iotsploit-host write 'GPIO:SET 2,1'
    iotsploit-host block-read 'DATA:READ? 64' --out adc.bin
    iotsploit-host repl
";

// ── CLI parsing ─────────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, Eq)]
enum BackendKind {
    Auto,
    Kernel,
    Raw,
    Tcp,
}

#[allow(dead_code)]
struct Cli {
    backend: BackendKind,
    /// Raw `--device` value; a scheme (`tcp://`, `usb://`) selects the backend.
    device: Option<String>,
    timeout_ms: u64,
    vid: Option<u16>,
    pid: Option<u16>,
    serial: Option<String>,
    command: String,
    args: Vec<String>,
}

fn parse_args(argv: Vec<String>) -> std::result::Result<Cli, String> {
    let mut backend = BackendKind::Auto;
    let mut device: Option<String> = None;
    let mut timeout_ms: u64 = 5000;
    let mut vid: Option<u16> = None;
    let mut pid: Option<u16> = None;
    let mut serial: Option<String> = None;
    let mut positionals: Vec<String> = Vec::new();

    let mut i = 0;
    while i < argv.len() {
        let a = &argv[i];
        match a.as_str() {
            "-h" | "--help" => {
                print!("{HELP}");
                std::process::exit(0);
            }
            "-V" | "--version" => {
                println!("iotsploit-host {}", env!("CARGO_PKG_VERSION"));
                std::process::exit(0);
            }
            "--backend" => {
                i += 1;
                let v = argv.get(i).ok_or("missing value for --backend")?;
                backend = match v.as_str() {
                    "auto" => BackendKind::Auto,
                    "kernel" => BackendKind::Kernel,
                    "raw" => BackendKind::Raw,
                    "tcp" => BackendKind::Tcp,
                    other => {
                        return Err(format!(
                            "unknown backend '{other}' (use auto|kernel|raw|tcp)"
                        ))
                    }
                };
            }
            "-d" | "--device" => {
                i += 1;
                let v = argv.get(i).ok_or("missing value for --device")?;
                device = Some(v.clone());
            }
            "--vid" => {
                i += 1;
                let v = argv.get(i).ok_or("missing value for --vid")?;
                vid = Some(u16::from_str_radix(v.trim_start_matches("0x"), 16)
                    .map_err(|_| format!("invalid VID: {v}"))?);
            }
            "--pid" => {
                i += 1;
                let v = argv.get(i).ok_or("missing value for --pid")?;
                pid = Some(u16::from_str_radix(v.trim_start_matches("0x"), 16)
                    .map_err(|_| format!("invalid PID: {v}"))?);
            }
            "--serial" => {
                i += 1;
                let v = argv.get(i).ok_or("missing value for --serial")?;
                serial = Some(v.clone());
            }
            "--timeout" => {
                i += 1;
                let v = argv.get(i).ok_or("missing value for --timeout")?;
                timeout_ms = v.parse().map_err(|_| format!("invalid --timeout: {v}"))?;
            }
            _ if a.starts_with("--device=") => {
                device = Some(a["--device=".len()..].to_string());
            }
            _ => positionals.push(a.clone()),
        }
        i += 1;
    }

    let command = positionals
        .first()
        .cloned()
        .ok_or_else(|| "no command given (see --help)".to_string())?;
    Ok(Cli {
        backend,
        device,
        timeout_ms,
        vid,
        pid,
        serial,
        command,
        args: positionals[1..].to_vec(),
    })
}

// ── Backend abstraction ─────────────────────────────────────────────────────

/// Runtime-selected transport backend.
enum Backend {
    #[cfg(feature = "kernel")]
    Kernel(UsbtmcKernel),
    #[cfg(feature = "raw-usb")]
    Raw(UsbtmcRaw),
    #[cfg(feature = "tcp")]
    Tcp(TcpTransport),
}

impl Transport for Backend {
    fn write_msg(&mut self, bytes: &[u8]) -> iotsploit_host::Result<()> {
        match self {
            #[cfg(feature = "kernel")]
            Backend::Kernel(t) => t.write_msg(bytes),
            #[cfg(feature = "raw-usb")]
            Backend::Raw(t) => t.write_msg(bytes),
            #[cfg(feature = "tcp")]
            Backend::Tcp(t) => t.write_msg(bytes),
            #[allow(unreachable_patterns)]
            _ => Err(Error::Device("no transport backend compiled in".into())),
        }
    }
    fn read_msg(&mut self, max_len: usize) -> iotsploit_host::Result<Vec<u8>> {
        match self {
            #[cfg(feature = "kernel")]
            Backend::Kernel(t) => t.read_msg(max_len),
            #[cfg(feature = "raw-usb")]
            Backend::Raw(t) => t.read_msg(max_len),
            #[cfg(feature = "tcp")]
            Backend::Tcp(t) => t.read_msg(max_len),
            #[allow(unreachable_patterns)]
            _ => Err(Error::Device("no transport backend compiled in".into())),
        }
    }
}

fn open_backend(cli: &Cli) -> iotsploit_host::Result<Backend> {
    // A scheme on --device names the transport unambiguously, so it wins over
    // --backend and over auto-detection.
    if let Some(dev) = cli.device.as_deref() {
        if dev.starts_with("tcp://") {
            return open_tcp(cli, &dev["tcp://".len()..]);
        }
        if dev.starts_with("usb://") {
            return open_raw_uri(cli, &dev["usb://".len()..]);
        }
    }

    match cli.backend {
        BackendKind::Kernel => open_kernel(cli),
        BackendKind::Raw => open_raw(cli),
        BackendKind::Tcp => match cli.device.as_deref() {
            Some(d) => open_tcp(cli, d.trim_start_matches("tcp://")),
            None => Err(Error::Device(
                "--backend tcp needs --device tcp://<host>[:<port>]".into(),
            )),
        },
        BackendKind::Auto => {
            // On Linux, try kernel first; fall back to raw.
            // Collect the actual error so the user sees *why* it failed
            // (e.g. "permission denied") instead of a generic message.
            let mut last_err: Option<Error> = None;

            #[cfg(feature = "kernel")]
            {
                match open_kernel(cli) {
                    Ok(b) => return Ok(b),
                    Err(e) => last_err = Some(e),
                }
            }
            #[cfg(feature = "raw-usb")]
            {
                match open_raw(cli) {
                    Ok(b) => return Ok(b),
                    Err(e) => last_err = Some(e),
                }
            }

            Err(last_err.unwrap_or_else(|| Error::Device(
                "auto-detect failed: no backend compiled in".into(),
            )))
        }
    }
}

#[cfg(feature = "kernel")]
fn open_kernel(cli: &Cli) -> iotsploit_host::Result<Backend> {
    let transport = match &cli.device {
        Some(p) => UsbtmcKernel::open(&PathBuf::from(p))?,
        None => UsbtmcKernel::auto_detect()?,
    };
    Ok(Backend::Kernel(transport))
}

#[cfg(not(feature = "kernel"))]
fn open_kernel(_cli: &Cli) -> iotsploit_host::Result<Backend> {
    Err(Error::Device("kernel backend not compiled in (enable `kernel` feature)".into()))
}

#[cfg(feature = "raw-usb")]
fn open_raw(cli: &Cli) -> iotsploit_host::Result<Backend> {
    let transport = if let (Some(vid), Some(pid)) = (cli.vid, cli.pid) {
        UsbtmcRaw::open_vid_pid(vid, pid)?
    } else if let Some(ref serial) = cli.serial {
        UsbtmcRaw::open_by_serial(serial)?
    } else {
        UsbtmcRaw::auto_detect()?
    };
    Ok(Backend::Raw(transport))
}

#[cfg(not(feature = "raw-usb"))]
fn open_raw(_cli: &Cli) -> iotsploit_host::Result<Backend> {
    Err(Error::Device("raw USB backend not compiled in (enable `raw-usb` feature)".into()))
}

/// `usb://<vid>:<pid>` with both fields in hex.
#[cfg(feature = "raw-usb")]
fn open_raw_uri(_cli: &Cli, spec: &str) -> iotsploit_host::Result<Backend> {
    let (v, p) = spec.split_once(':').ok_or_else(|| {
        Error::Device(format!("expected usb://<vid>:<pid>, got usb://{spec}"))
    })?;
    let vid = u16::from_str_radix(v.trim_start_matches("0x"), 16)
        .map_err(|_| Error::Device(format!("invalid VID: {v}")))?;
    let pid = u16::from_str_radix(p.trim_start_matches("0x"), 16)
        .map_err(|_| Error::Device(format!("invalid PID: {p}")))?;
    Ok(Backend::Raw(UsbtmcRaw::open_vid_pid(vid, pid)?))
}

#[cfg(not(feature = "raw-usb"))]
fn open_raw_uri(_cli: &Cli, _spec: &str) -> iotsploit_host::Result<Backend> {
    Err(Error::Device("raw USB backend not compiled in (enable `raw-usb` feature)".into()))
}

#[cfg(feature = "tcp")]
fn open_tcp(cli: &Cli, addr: &str) -> iotsploit_host::Result<Backend> {
    let t = TcpTransport::connect(addr, Duration::from_millis(cli.timeout_ms))?;
    Ok(Backend::Tcp(t))
}

#[cfg(not(feature = "tcp"))]
fn open_tcp(_cli: &Cli, _addr: &str) -> iotsploit_host::Result<Backend> {
    Err(Error::Device("tcp backend not compiled in (enable `tcp` feature)".into()))
}

fn open_session(cli: &Cli) -> iotsploit_host::Result<ScpiSession<Backend>> {
    let transport = open_backend(cli)?;
    Ok(ScpiSession::new(transport).with_read_size(8192).with_max_block_len(1 << 20))
}

// ── Profile resolution ─────────────────────────────────────────────────────

/// Fetch the device's descriptor (`SYSTem:HELP:DESCription?`) as a [`Profile`].
///
/// Byte offset of a named field in a `name:type[:unit],...` schema.
///
/// Only fixed-width scalar types are walked; anything else (a trailing
/// `bytesN` blob, say) ends the walk, which is fine because the counters this
/// is used for come first by convention.
#[cfg(feature = "tcp")]
#[cfg(feature = "tcp")]
fn stream_host_of(device: Option<&str>) -> String {
    let d = device.unwrap_or("");
    let rest = d.strip_prefix("tcp://").unwrap_or(d);
    let host = rest.split(':').next().unwrap_or("");
    if host.is_empty() { "127.0.0.1".to_string() } else { host.to_string() }
}

fn field_offset(fields: &str, want: &str) -> Option<usize> {
    let mut off = 0usize;
    for field in fields.split(',') {
        let mut it = field.split(':');
        let name = it.next()?.trim();
        let ty = it.next()?.trim();
        let size = match ty {
            "u8" | "i8" => 1,
            "u16" | "i16" => 2,
            "u32" | "i32" | "f32" => 4,
            "u64" | "i64" | "f64" => 8,
            _ => return None,
        };
        if name == want {
            return Some(off);
        }
        off += size;
    }
    None
}

#[cfg(feature = "tcp")]
fn hex(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

/// The device is the single source of truth for its command/workflow metadata,
/// so there are no local profile files. Firmware that does not yet implement the
/// descriptor query yields a clear, actionable error.
fn fetch_profile(
    s: &mut ScpiSession<Backend>,
) -> iotsploit_host::Result<iotsploit_host::descriptor::Profile> {
    iotsploit_host::descriptor::fetch(s)?.ok_or_else(|| {
        Error::Device(
            "device does not serve SYSTem:HELP:DESCription? — update its firmware to expose \
             its descriptor"
                .into(),
        )
    })
}

// ── Command dispatch ───────────────────────────────────────────────────────

fn run(cli: Cli) -> Result<(), Error> {
    match cli.command.as_str() {
        "list" => {
            for node in list_nodes() {
                println!("{}", node.display());
            }
            Ok(())
        }
        "idn" => {
            let mut s = open_session(&cli)?;
            let idn = s.idn()?;
            println!("{idn}");
            Ok(())
        }
        #[cfg(feature = "tcp")]
        "stream" => {
            use iotsploit_host::dataplane::{self, DataPlane, GapTracker};
            let limit: usize = cli.args.first().and_then(|a| a.parse().ok()).unwrap_or(0);

            // The control plane is the single source of truth for where the
            // data plane is and what it emits; nothing here is hardcoded.
            let mut s = open_session(&cli)?;
            let (port, fmt) = match dataplane::discover(&mut s)? {
                Some(v) => v,
                None => {
                    eprintln!("device has no data plane (SYSTem:STReam:PORT? returned 0)");
                    return Ok(());
                }
            };
            eprintln!("stream: port {port}, v{} stride {}", fmt.version, fmt.stride);
            eprintln!("fields: {}", fmt.fields);

            let host = stream_host_of(cli.device.as_deref());
            let mut dp = DataPlane::connect((host.as_str(), port), fmt.stride,
                                            Duration::from_millis(cli.timeout_ms))?;
            s.write("SYSTem:STReam:STARt")?;

            // `dropped` is a u64 at a schema-declared offset. Locate it rather
            // than assuming, so a different source still reports gaps.
            let offset = field_offset(&fmt.fields, "dropped");
            let mut gaps = offset.map(GapTracker::new);
            let mut n = 0usize;
            // Piping a record stream into `head` is normal usage, and Rust's
            // println! panics on EPIPE. Write explicitly and treat a closed
            // downstream as a reason to stop, not an error.
            let stdout = std::io::stdout();
            let mut out = stdout.lock();
            loop {
                match dp.next_record()? {
                    Some(rec) => {
                        if let Some(g) = gaps.as_mut() {
                            let lost = g.observe(&rec);
                            if lost > 0 {
                                eprintln!("-- {lost} record(s) lost before #{n} --");
                            }
                        }
                        use std::io::Write as _;
                        if writeln!(out, "{}", hex(&rec)).is_err() {
                            break;
                        }
                        n += 1;
                        if limit != 0 && n >= limit {
                            break;
                        }
                    }
                    None => break,
                }
            }
            let _ = s.write("SYSTem:STReam:STOP");
            eprintln!("{n} records, {} lost",
                      gaps.as_ref().map(|g| g.total_lost()).unwrap_or(0));
            Ok(())
        }
        "caps" => {
            let mut s = open_session(&cli)?;
            let caps = s.caps()?;
            print_caps(&caps);
            Ok(())
        }
        "headers" => {
            let mut s = open_session(&cli)?;
            let h = headers::fetch_headers(&mut s, None)?;
            for line in h {
                println!("{line}");
            }
            Ok(())
        }
        "describe" => {
            let mut s = open_session(&cli)?;
            match iotsploit_host::descriptor::fetch(&mut s)? {
                Some(desc) => {
                    if let Some(idn) = &desc.device.idn {
                        println!("idn    : {idn}");
                    }
                    if !desc.device.name.is_empty() {
                        println!("name   : {}", desc.device.name);
                    }
                    println!("commands: {}", desc.commands.len());
                    println!("workflows: {}", desc.workflows.len());
                    for wf in &desc.workflows {
                        println!("  - {} ({:?})", wf.name, wf.workflow_type);
                    }
                }
                None => {
                    eprintln!("device does not support SYSTem:HELP:DESCription?");
                }
            }
            Ok(())
        }
        "query" => {
            let cmd = cli
                .args
                .first()
                .ok_or_else(|| Error::Device("query requires a command argument".into()))?;
            let mut s = open_session(&cli)?;
            let resp = s.query(cmd)?;
            println!("{resp}");
            Ok(())
        }
        "write" => {
            let cmd = cli
                .args
                .first()
                .ok_or_else(|| Error::Device("write requires a command argument".into()))?;
            let mut s = open_session(&cli)?;
            s.write(cmd)?;
            Ok(())
        }
        "block-read" => {
            let cmd = cli
                .args
                .first()
                .ok_or_else(|| Error::Device("block-read requires a command argument".into()))?;
            let out = match cli.args.get(1) {
                Some(v) if v == "--out" || v.starts_with("--out=") => {
                    let val = if let Some(eq) = v.find('=') {
                        v[eq + 1..].to_string()
                    } else {
                        cli.args
                            .get(2)
                            .cloned()
                            .ok_or_else(|| Error::Device("missing value for --out".into()))?
                    };
                    Some(val)
                }
                _ => None,
            };
            let mut s = open_session(&cli)?;
            let payload = s.query_block(cmd)?;
            match out {
                None => {
                    let mut out = io::stdout().lock();
                    out.write_all(&payload)?;
                }
                Some(path) => {
                    std::fs::write(&path, &payload)?;
                    eprintln!("wrote {} bytes to {}", payload.len(), path);
                }
            }
            Ok(())
        }
        "errors" => {
            let mut s = open_session(&cli)?;
            let errs = s.drain_errors()?;
            if errs.is_empty() {
                println!("(no errors)");
            } else {
                for e in errs {
                    println!("{},\"{}\"", e.code, e.message);
                }
            }
            Ok(())
        }
        "workflow" => {
            let wf_name = cli
                .args
                .first()
                .ok_or_else(|| Error::Device("workflow requires a name (e.g. wifi-scan)".into()))?;
            let wf_params: Vec<String> = cli.args[1..].to_vec();

            let mut s = open_session(&cli)?;
            let profile = fetch_profile(&mut s)?;
            iotsploit_host::workflow::run_workflow(&mut s, &profile, wf_name, &wf_params)
        }
        "profile" => {
            let mut s = open_session(&cli)?;
            let p = fetch_profile(&mut s)?;
            if let Some(idn) = &p.device.idn {
                println!("idn      : {idn}");
            }
            if !p.device.name.is_empty() {
                println!("device   : {}", p.device.name);
            }
            println!("commands : {}", p.commands.len());
            for c in &p.commands {
                println!("  {} [{}] {}", c.pattern, c.kind, c.summary);
            }
            println!("workflows: {}", p.workflows.len());
            for w in &p.workflows {
                println!("  {} ({:?}) {}", w.name, w.workflow_type, w.summary);
            }
            Ok(())
        }
        "repl" => repl(&cli),
        other => Err(Error::Device(format!("unknown command '{other}' (see --help)"))),
    }
}

fn list_nodes() -> Vec<PathBuf> {
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
    nodes.sort();
    nodes
}

fn print_caps(caps: &caps::Capabilities) {
    println!("raw      : {}", caps.raw);
    println!("proto    : {:?}", caps.proto);
    println!("mtu      : {:?}", caps.mtu);
    println!("maxblock : {:?}", caps.max_block);
    println!("features : {:?}", caps.features);
    if !caps.unknown.is_empty() {
        println!("unknown  : {:?}", caps.unknown);
    }
    if !caps.parse_errors.is_empty() {
        let pairs: Vec<String> = caps
            .parse_errors
            .iter()
            .map(|(k, v)| format!("{k}=\"{v}\""))
            .collect();
        eprintln!("warning  : unparseable numeric fields: {}", pairs.join(", "));
    }
}

fn repl(cli: &Cli) -> Result<(), Error> {
    let mut s = open_session(cli)?;
    let stdin = io::stdin();
    let mut line = String::new();
    println!("iotsploit-host repl - type SCPI commands, Ctrl-D to exit");
    loop {
        line.clear();
        io::stdout().flush().ok();
        print!("> ");
        io::stdout().flush().ok();
        match stdin.read_line(&mut line) {
            Ok(0) => {
                println!();
                return Ok(());
            }
            Ok(_) => {}
            Err(e) => return Err(Error::Io(e)),
        }
        let cmd = line.trim();
        if cmd.is_empty() {
            continue;
        }
        if cmd.eq_ignore_ascii_case("exit") || cmd.eq_ignore_ascii_case("quit") {
            return Ok(());
        }
        let result = if cmd.ends_with('?') {
            s.query(cmd).map(|t| Some(t))
        } else {
            s.write(cmd).map(|_| None)
        };
        match result {
            Ok(Some(text)) => println!("{text}"),
            Ok(None) => println!("ok"),
            Err(e) => eprintln!("error: {e}"),
        }
    }
}

fn main() -> ExitCode {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let cli = match parse_args(argv) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("iotsploit-host: {e}");
            eprintln!("\n{HELP}");
            return ExitCode::from(2);
        }
    };
    match run(cli) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("iotsploit-host: {e}");
            ExitCode::from(1)
        }
    }
}
