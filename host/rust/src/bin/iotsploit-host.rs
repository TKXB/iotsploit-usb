//! `iotsploit-host` CLI binary.
//!
//! A command-line tool for talking to `iotsploit-usb` devices over SCPI/USBTMC.
//! Supports multiple transport backends (kernel `/dev/usbtmcN`, raw `nusb`),
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

const HELP: &str = "\
iotsploit-host - generic SCPI-over-USBTMC host for iotsploit-usb

USAGE:
    iotsploit-host [options] <command> [args]

GLOBAL OPTIONS:
    --backend auto|kernel|raw   Transport backend (default: auto)
    -d, --device <path>         /dev/usbtmcN node (kernel backend)
    --vid <hex> --pid <hex>     USB VID/PID for raw backend (e.g. 1209 0001)
    --serial <string>           USB serial number (raw backend)
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
    iotsploit-host workflow wifi-scan
    iotsploit-host workflow ble-scan 8
    iotsploit-host --backend raw --vid 1209 --pid 0001 idn
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
}

#[allow(dead_code)]
struct Cli {
    backend: BackendKind,
    device: Option<PathBuf>,
    vid: Option<u16>,
    pid: Option<u16>,
    serial: Option<String>,
    command: String,
    args: Vec<String>,
}

fn parse_args(argv: Vec<String>) -> std::result::Result<Cli, String> {
    let mut backend = BackendKind::Auto;
    let mut device: Option<PathBuf> = None;
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
                    other => return Err(format!("unknown backend '{other}' (use auto|kernel|raw)")),
                };
            }
            "-d" | "--device" => {
                i += 1;
                let v = argv.get(i).ok_or("missing value for --device")?;
                device = Some(PathBuf::from(v));
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
            _ if a.starts_with("--device=") => {
                device = Some(PathBuf::from(&a["--device=".len()..]));
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
}

impl Transport for Backend {
    fn write_msg(&mut self, bytes: &[u8]) -> iotsploit_host::Result<()> {
        match self {
            #[cfg(feature = "kernel")]
            Backend::Kernel(t) => t.write_msg(bytes),
            #[cfg(feature = "raw-usb")]
            Backend::Raw(t) => t.write_msg(bytes),
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
            #[allow(unreachable_patterns)]
            _ => Err(Error::Device("no transport backend compiled in".into())),
        }
    }
}

fn open_backend(cli: &Cli) -> iotsploit_host::Result<Backend> {
    match cli.backend {
        BackendKind::Kernel => open_kernel(cli),
        BackendKind::Raw => open_raw(cli),
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
        Some(p) => UsbtmcKernel::open(p)?,
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

fn open_session(cli: &Cli) -> iotsploit_host::Result<ScpiSession<Backend>> {
    let transport = open_backend(cli)?;
    Ok(ScpiSession::new(transport).with_read_size(8192).with_max_block_len(1 << 20))
}

// ── Profile resolution ─────────────────────────────────────────────────────

/// Fetch the device's descriptor (`SYSTem:HELP:DESCription?`) as a [`Profile`].
///
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
            s.write_and_drain(cmd)?;
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
            s.write_and_drain(cmd).map(|_| None)
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
