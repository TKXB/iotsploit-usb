//! `iotsploit-host` CLI binary.
//!
//! A command-line tool for talking to `iotsploit-usb` devices over SCPI.
//! Supports multiple transport backends (kernel `/dev/usbtmcN`, raw `nusb`,
//! raw SCPI over TCP), descriptor-driven help, and workflow execution.

use std::io::{self, Write};
use std::process::ExitCode;

use iotsploit_host::{
    caps,
    descriptor::{self, Profile},
    discover::{self, Address},
    headers,
    session::{Checked, Output, ScpiSession},
    transport::Transport,
    Error,
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
iotsploit-host - control iotsploit-usb devices over USB or TCP

USAGE:
    iotsploit-host [-d <device>] <command> [args]

COMMANDS:
    devices               List connected USB devices, numbered, with identity
    info                  What the device is: identity, address, capabilities
    help [name]           What the device can do; with a name, the details of
                          one command or workflow
    send <command>        Send one SCPI command; print the reply or the error
    workflow <name> [..]  Run a workflow the device describes
    stream [count]        Read records from the device's data plane (TCP)
    repl                  Interactive prompt; each line is sent like `send`
    errors                Read and clear the device's error queue

OPTIONS:
    -d, --device <dev>    A number from `devices`, usbtmc:N,
                          usb:<vid>:<pid>[/<serial>] or tcp://<host>[:<port>].
                          Default: $IOTSPLOIT_DEVICE, else the only USB device
    -o, --out <file>      Save a binary reply to a file instead of stdout
    --timeout <ms>        Read timeout for TCP and raw USB (default: 5000)
    -h, --help            Show this help
    -V, --version         Show the version

EXIT STATUS:
    0 ok   1 the device reported an error   2 usage error   3 could not connect

EXAMPLES:
    iotsploit-host devices
    iotsploit-host -d 2 info
    iotsploit-host help GPIO:SET
    iotsploit-host send '*IDN?'
    iotsploit-host send GPIO:SET 2,1
    iotsploit-host send 'DATA:READ? 64' -o adc.bin
    iotsploit-host workflow wifi-scan
    iotsploit-host -d tcp://192.168.4.1 info

Older commands (idn, caps, headers, describe, profile, query, write,
block-read, list) and options (--backend, --vid, --pid, --serial) still work
and print the name that replaces them.
";

// ── Failures and exit codes ─────────────────────────────────────────────────

/// A failed run: what to print and which exit status it maps to.
struct Fail {
    code: u8,
    /// Empty when the message was already printed (device errors).
    msg: String,
}

const EXIT_DEVICE: u8 = 1;
const EXIT_USAGE: u8 = 2;
const EXIT_CONNECT: u8 = 3;

impl From<Error> for Fail {
    fn from(e: Error) -> Self {
        Fail { code: EXIT_DEVICE, msg: e.to_string() }
    }
}

impl From<io::Error> for Fail {
    fn from(e: io::Error) -> Self {
        Fail::from(Error::Io(e))
    }
}

fn usage(msg: impl Into<String>) -> Fail {
    Fail { code: EXIT_USAGE, msg: msg.into() }
}

fn connect(e: Error) -> Fail {
    Fail { code: EXIT_CONNECT, msg: e.to_string() }
}

type CliResult = std::result::Result<(), Fail>;

/// Tell the user an old name still works and what replaces it.
fn renamed(old: &str, new: &str) {
    eprintln!("note: `{old}` is now `{new}`; the old name will be removed in a later release");
}

// ── CLI parsing ─────────────────────────────────────────────────────────────

/// Options kept only so existing scripts keep working.
#[derive(Default)]
struct Legacy {
    backend: Option<String>,
    vid: Option<u16>,
    pid: Option<u16>,
    serial: Option<String>,
}

struct Cli {
    device: Option<String>,
    out: Option<String>,
    timeout_ms: u64,
    legacy: Legacy,
    command: String,
    args: Vec<String>,
}

fn parse_args(argv: Vec<String>) -> std::result::Result<Cli, String> {
    let mut device = None;
    let mut out = None;
    let mut timeout_ms: u64 = 5000;
    let mut legacy = Legacy::default();
    let mut positionals: Vec<String> = Vec::new();

    let mut it = argv.into_iter();
    while let Some(a) = it.next() {
        // `--name=value` and `--name value` are both accepted.
        let (name, inline) = match a.split_once('=') {
            Some((n, v)) if n.starts_with("--") => (n.to_string(), Some(v.to_string())),
            _ => (a.clone(), None),
        };
        let mut value = |what: &str| -> std::result::Result<String, String> {
            inline.clone().or_else(|| it.next()).ok_or(format!("missing value for {what}"))
        };
        match name.as_str() {
            "-h" | "--help" => {
                print!("{HELP}");
                std::process::exit(0);
            }
            "-V" | "--version" => {
                println!("iotsploit-host {}", env!("CARGO_PKG_VERSION"));
                std::process::exit(0);
            }
            "-d" | "--device" => device = Some(value("--device")?),
            "-o" | "--out" => out = Some(value("--out")?),
            "--timeout" => {
                let v = value("--timeout")?;
                timeout_ms = v.parse().map_err(|_| format!("invalid --timeout: {v}"))?;
            }
            "--backend" => legacy.backend = Some(value("--backend")?),
            "--vid" => legacy.vid = Some(discover::parse_hex(&value("--vid")?)?),
            "--pid" => legacy.pid = Some(discover::parse_hex(&value("--pid")?)?),
            "--serial" => legacy.serial = Some(value("--serial")?),
            _ => positionals.push(a),
        }
    }

    let mut positionals = positionals.into_iter();
    let command = positionals.next().ok_or("no command given (see --help)")?;
    Ok(Cli { device, out, timeout_ms, legacy, command, args: positionals.collect() })
}

// ── Choosing and opening a device ───────────────────────────────────────────

/// What to open. `RawSerial` and `RawAuto` exist only for the legacy flags.
enum Target {
    Addr(Address),
    RawSerial(String),
    RawAuto,
}

fn resolve(cli: &Cli) -> std::result::Result<Target, Fail> {
    let l = &cli.legacy;
    if l.backend.is_some() || l.vid.is_some() || l.pid.is_some() || l.serial.is_some() {
        renamed("--backend/--vid/--pid/--serial", "-d <device>");
    }
    match (l.vid, l.pid, &l.serial) {
        (Some(vid), Some(pid), serial) => {
            return Ok(Target::Addr(Address::Usb { vid, pid, serial: serial.clone() }))
        }
        (None, None, Some(s)) => return Ok(Target::RawSerial(s.clone())),
        (None, None, None) => {}
        _ => return Err(usage("--vid and --pid go together")),
    }

    let spec = cli.device.clone().or_else(|| std::env::var("IOTSPLOIT_DEVICE").ok());
    match (l.backend.as_deref(), spec.as_deref()) {
        (Some("raw"), None) => return Ok(Target::RawAuto),
        (Some("tcp"), Some(d)) if !d.starts_with("tcp://") => {
            return parse_address(&format!("tcp://{d}")).map(Target::Addr)
        }
        (Some("tcp"), None) => return Err(usage("--backend tcp needs -d tcp://<host>[:<port>]")),
        (Some(b), _) if !matches!(b, "auto" | "kernel" | "raw" | "tcp") => {
            return Err(usage(format!("unknown backend '{b}'")))
        }
        _ => {}
    }

    if let Some(spec) = spec {
        if let Ok(n) = spec.parse::<usize>() {
            let found = discover::list();
            return match found.get(n.wrapping_sub(1)) {
                Some(a) => Ok(Target::Addr(a.clone())),
                None => Err(usage(format!(
                    "no device number {n}; `devices` lists {} device(s)",
                    found.len()
                ))),
            };
        }
        return parse_address(&spec).map(Target::Addr);
    }

    let mut found = discover::list();
    match found.len() {
        0 => Err(connect(Error::Device(
            "no USB device found; check the cable, or name a network device with \
             -d tcp://<host>"
                .into(),
        ))),
        1 => Ok(Target::Addr(found.remove(0))),
        n => {
            let list: Vec<String> = found.iter().enumerate().map(|(i, a)| format!("{} {a}", i + 1)).collect();
            Err(usage(format!(
                "{n} devices connected; choose one with -d <number>: {}",
                list.join(", ")
            )))
        }
    }
}

fn parse_address(spec: &str) -> std::result::Result<Address, Fail> {
    discover::parse(spec).map_err(usage)
}

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

#[cfg(not(all(feature = "kernel", feature = "tcp", feature = "raw-usb")))]
fn not_built(what: &str, feature: &str) -> Error {
    Error::Device(format!("{what} support is not built in (rebuild with `--features {feature}`)"))
}

#[allow(unused_variables)]
fn open_target(target: &Target, timeout_ms: u64) -> iotsploit_host::Result<Backend> {
    match target {
        Target::Addr(Address::Kernel(path)) => {
            #[cfg(feature = "kernel")]
            return UsbtmcKernel::open(path).map(Backend::Kernel);
            #[cfg(not(feature = "kernel"))]
            return Err(not_built("/dev/usbtmc", "kernel"));
        }
        Target::Addr(Address::Tcp(hp)) => {
            #[cfg(feature = "tcp")]
            return TcpTransport::connect(hp, Duration::from_millis(timeout_ms)).map(Backend::Tcp);
            #[cfg(not(feature = "tcp"))]
            return Err(not_built("TCP", "tcp"));
        }
        #[cfg(feature = "raw-usb")]
        Target::Addr(Address::Usb { vid, pid, serial }) => {
            let t = match serial {
                Some(s) => UsbtmcRaw::open_by_serial(s)?,
                None => UsbtmcRaw::open_vid_pid(*vid, *pid)?,
            };
            Ok(Backend::Raw(t.with_timeout(timeout_ms)))
        }
        #[cfg(feature = "raw-usb")]
        Target::Addr(Address::UsbBus { bus, addr, .. }) => {
            Ok(Backend::Raw(UsbtmcRaw::open_by_bus_address(*bus, *addr)?.with_timeout(timeout_ms)))
        }
        #[cfg(feature = "raw-usb")]
        Target::RawSerial(s) => Ok(Backend::Raw(UsbtmcRaw::open_by_serial(s)?.with_timeout(timeout_ms))),
        #[cfg(feature = "raw-usb")]
        Target::RawAuto => Ok(Backend::Raw(UsbtmcRaw::auto_detect()?.with_timeout(timeout_ms))),
        #[cfg(not(feature = "raw-usb"))]
        _ => Err(not_built("raw USB", "raw-usb")),
    }
}

fn session_for(backend: Backend) -> ScpiSession<Backend> {
    ScpiSession::new(backend).with_read_size(8192).with_max_block_len(1 << 20)
}

/// Resolve and open the selected device. Returns its address for display.
fn open(cli: &Cli) -> std::result::Result<(ScpiSession<Backend>, String), Fail> {
    let target = resolve(cli)?;
    let label = match &target {
        Target::Addr(a) => a.to_string(),
        Target::RawSerial(s) => format!("usb serial {s}"),
        Target::RawAuto => "usb (auto)".to_string(),
    };
    let backend = open_target(&target, cli.timeout_ms).map_err(connect)?;
    Ok((session_for(backend), label))
}

// ── Descriptor and header helpers ───────────────────────────────────────────

/// The device's descriptor, or `None` when its firmware does not serve one.
/// Uses [`ScpiSession::send_checked`], so an unsupported query reports an
/// error instead of waiting for a reply that never comes.
fn read_descriptor(s: &mut ScpiSession<Backend>) -> std::result::Result<Option<Profile>, Fail> {
    let c = s.send_checked(descriptor::DESC_QUERY)?;
    match c.output {
        Output::Block(p) if !p.is_empty() => {
            let text = String::from_utf8(p).map_err(|e| Fail::from(Error::Scpi {
                cmd: descriptor::DESC_QUERY.into(),
                msg: format!("descriptor is not valid UTF-8: {e}"),
            }))?;
            descriptor::parse_str(&text).map(Some).map_err(|e| {
                Fail::from(Error::Scpi { cmd: descriptor::DESC_QUERY.into(), msg: e.to_string() })
            })
        }
        _ => Ok(None),
    }
}

/// SCPI short form of one pattern segment: its uppercase letters, digits and
/// trailing `?` (`STATe?` → `STAT?`).
fn short_form(segment: &str) -> String {
    segment.chars().filter(|c| !c.is_ascii_lowercase()).collect()
}

/// Whether `input` names `pattern`, using SCPI rules: each segment may be the
/// long or the short form, in any case; `?` must match.
fn scpi_matches(pattern: &str, input: &str) -> bool {
    let p: Vec<&str> = pattern.trim_start_matches(':').split(':').collect();
    let i: Vec<&str> = input.trim_start_matches(':').split(':').collect();
    p.len() == i.len()
        && p.iter().zip(&i).all(|(ps, is)| {
            is.eq_ignore_ascii_case(ps) || is.eq_ignore_ascii_case(&short_form(ps))
        })
}

fn edit_distance(a: &str, b: &str) -> usize {
    let a: Vec<char> = a.chars().collect();
    let b: Vec<char> = b.chars().collect();
    let mut prev: Vec<usize> = (0..=b.len()).collect();
    for (i, ca) in a.iter().enumerate() {
        let mut cur = vec![i + 1];
        for (j, cb) in b.iter().enumerate() {
            let sub = prev[j] + usize::from(ca != cb);
            cur.push(sub.min(prev[j + 1] + 1).min(cur[j] + 1));
        }
        prev = cur;
    }
    prev[b.len()]
}

/// The registered header closest to a mistyped one, if any is close enough.
fn closest<'a>(input: &str, patterns: &'a [String]) -> Option<&'a str> {
    let want = input.to_ascii_uppercase();
    let short = |p: &str| p.split(':').map(short_form).collect::<Vec<_>>().join(":");
    patterns
        .iter()
        .map(|p| {
            let d = edit_distance(&want, &p.to_ascii_uppercase()).min(edit_distance(&want, &short(p)));
            (d, p.as_str())
        })
        .filter(|(d, _)| *d <= (want.len() / 4).max(2))
        .min_by_key(|(d, _)| *d)
        .map(|(_, p)| p)
}

/// First token of a command line, without a leading `:`.
fn header_of(cmd: &str) -> &str {
    cmd.split_whitespace().next().unwrap_or("").trim_start_matches(':')
}

/// Print device errors from a checked command; `true` when there were any.
fn report_errors(s: &mut ScpiSession<Backend>, cmd: &str, c: &Checked) -> bool {
    for e in &c.errors {
        eprintln!("error {} {}: {}", e.code, e.message, header_of(cmd));
        if e.code == -113 {
            if let Ok(h) = headers::fetch_headers(s, None) {
                if let Some(best) = closest(header_of(cmd), &h) {
                    eprintln!("  did you mean `{best}`");
                }
            }
        }
    }
    !c.errors.is_empty()
}

/// Whether `cmd` must bypass the appended error check.
fn is_error_query(cmd: &str) -> bool {
    scpi_matches("SYSTem:ERRor?", header_of(cmd)) || scpi_matches("SYSTem:ERRor:NEXT?", header_of(cmd))
}

// ── Commands ────────────────────────────────────────────────────────────────

fn run(cli: Cli) -> CliResult {
    match cli.command.as_str() {
        "devices" => cmd_devices(&cli),
        "info" => cmd_info(&cli),
        "help" => cmd_help(&cli),
        "send" => cmd_send(&cli),
        "workflow" => cmd_workflow(&cli),
        "errors" => cmd_errors(&cli),
        "repl" => cmd_repl(&cli),
        #[cfg(feature = "tcp")]
        "stream" => cmd_stream(&cli),
        // Old names: same behaviour as before, plus a hint.
        "list" => {
            renamed("list", "devices");
            for a in discover::list() {
                if let Address::Kernel(p) = a {
                    println!("{}", p.display());
                }
            }
            Ok(())
        }
        "idn" => {
            renamed("idn", "info");
            println!("{}", open(&cli)?.0.idn()?);
            Ok(())
        }
        "caps" => {
            renamed("caps", "info");
            print_caps(&open(&cli)?.0.caps()?);
            Ok(())
        }
        "headers" => {
            renamed("headers", "help");
            for line in headers::fetch_headers(&mut open(&cli)?.0, None)? {
                println!("{line}");
            }
            Ok(())
        }
        "describe" | "profile" => {
            renamed(&cli.command, "help");
            cmd_help(&cli)
        }
        "query" => {
            renamed("query", "send");
            let cmd = one_arg(&cli, "query")?;
            println!("{}", open(&cli)?.0.query(&cmd)?);
            Ok(())
        }
        "write" => {
            renamed("write", "send");
            let cmd = one_arg(&cli, "write")?;
            open(&cli)?.0.write(&cmd)?;
            Ok(())
        }
        "block-read" => {
            renamed("block-read", "send");
            let cmd = one_arg(&cli, "block-read")?;
            let payload = open(&cli)?.0.query_block(&cmd)?;
            write_payload(&payload, cli.out.as_deref())
        }
        other => Err(usage(format!("unknown command '{other}' (see --help)"))),
    }
}

fn one_arg(cli: &Cli, what: &str) -> std::result::Result<String, Fail> {
    cli.args.first().cloned().ok_or_else(|| usage(format!("{what} needs a SCPI command")))
}

fn write_payload(payload: &[u8], out: Option<&str>) -> CliResult {
    match out {
        Some(path) => {
            std::fs::write(path, payload)?;
            eprintln!("wrote {} bytes to {path}", payload.len());
        }
        None => io::stdout().lock().write_all(payload)?,
    }
    Ok(())
}

fn cmd_devices(cli: &Cli) -> CliResult {
    let found = discover::list();
    if found.is_empty() {
        println!("no USB devices found");
        println!("network devices are not discovered; use -d tcp://<host>[:<port>]");
        return Ok(());
    }
    let width = found.iter().map(|a| a.to_string().len()).max().unwrap_or(0).max(7);
    println!("{:<3}{:<width$}  IDENTITY", "#", "ADDRESS");
    for (i, a) in found.iter().enumerate() {
        let identity = match open_target(&Target::Addr(a.clone()), cli.timeout_ms) {
            Ok(b) => session_for(b).idn().unwrap_or_else(|e| format!("(no reply: {e})")),
            Err(e) => format!("(cannot open: {e})"),
        };
        println!("{:<3}{:<width$}  {identity}", i + 1, a.to_string());
    }
    Ok(())
}

fn cmd_info(cli: &Cli) -> CliResult {
    let (mut s, address) = open(cli)?;
    let idn = s.idn()?;
    let caps = s.caps()?;
    let desc = read_descriptor(&mut s)?;
    let num = |v: Option<usize>| v.map_or("?".to_string(), |n| n.to_string());
    println!("device     {idn}");
    println!("address    {address}");
    println!(
        "protocol   {}   mtu {}   max block {}",
        caps.proto.map_or("?".to_string(), |p| p.to_string()),
        num(caps.mtu),
        num(caps.max_block)
    );
    if !caps.features.is_empty() {
        println!("features   {}", caps.features.join(", "));
    }
    match desc {
        Some(p) => {
            let stream = p.commands.iter().any(|c| scpi_matches(&c.pattern, "SYST:STR:PORT?"));
            println!(
                "commands   {}   workflows {}   stream {}",
                p.commands.len(),
                p.workflows.len(),
                if stream { "yes" } else { "no" }
            );
        }
        None => {
            let n = headers::fetch_headers(&mut s, None)?.len();
            println!("commands   {n} (no descriptor: `help` shows names only)");
        }
    }
    if !caps.parse_errors.is_empty() {
        warn_caps(&caps);
    }
    Ok(())
}

fn cmd_help(cli: &Cli) -> CliResult {
    let (mut s, _) = open(cli)?;
    let Some(profile) = read_descriptor(&mut s)? else {
        let h = headers::fetch_headers(&mut s, None)?;
        if let Some(name) = cli.args.first() {
            return match h.iter().find(|p| scpi_matches(p, name)) {
                Some(p) => {
                    println!("{p}");
                    println!("  this device does not describe its commands, so no details are available");
                    Ok(())
                }
                None => Err(not_found(name, "command", &h)),
            };
        }
        for p in &h {
            println!("{p}");
        }
        eprintln!("\nthis device does not describe its commands; update its firmware for details and workflows");
        return Ok(());
    };

    match cli.args.first() {
        None => {
            print_help_overview(&profile);
            Ok(())
        }
        Some(name) => {
            if let Some(w) = profile.workflow(name) {
                print_workflow_help(&profile, w);
                Ok(())
            } else if let Some(c) = profile.commands.iter().find(|c| scpi_matches(&c.pattern, name)) {
                print_command_help(c);
                Ok(())
            } else {
                let mut names: Vec<String> = profile.commands.iter().map(|c| c.pattern.clone()).collect();
                names.extend(profile.workflows.iter().map(|w| w.name.clone()));
                Err(not_found(name, "command or workflow", &names))
            }
        }
    }
}

fn not_found(name: &str, what: &str, names: &[String]) -> Fail {
    match closest(name, names) {
        Some(best) => usage(format!("`{name}` is not a {what} on this device; did you mean `{best}`")),
        None => usage(format!("`{name}` is not a {what} on this device (run `help` for the list)")),
    }
}

/// `GPIO:SET <pin>,<value>` style usage line from the declared parameters.
fn usage_line(c: &descriptor::CommandDesc) -> String {
    if c.params.is_empty() {
        return c.pattern.clone();
    }
    let args: Vec<String> = c
        .params
        .iter()
        .map(|p| if p.required { format!("<{}>", p.name) } else { format!("[{}]", p.name) })
        .collect();
    format!("{} {}", c.pattern, args.join(","))
}

fn print_help_overview(p: &Profile) {
    if let Some(idn) = &p.device.idn {
        println!("{idn}\n");
    }
    if !p.commands.is_empty() {
        println!("COMMANDS (send with: iotsploit-host send <command>)");
        let lines: Vec<(String, &str)> = p.commands.iter().map(|c| (usage_line(c), c.summary.as_str())).collect();
        let w = lines.iter().map(|(u, _)| u.len()).max().unwrap_or(0);
        for (u, summary) in lines {
            println!("  {u:<w$}  {summary}");
        }
    }
    // Old names kept for compatibility are not advertised.
    let current: Vec<&descriptor::WorkflowDesc> = p.workflows.iter().filter(|w| w.renamed_to.is_none()).collect();
    if !current.is_empty() {
        println!("\nWORKFLOWS (run with: iotsploit-host workflow <name>)");
        let w = current.iter().map(|wf| wf.name.len()).max().unwrap_or(0);
        for wf in current {
            println!("  {:<w$}  {}", wf.name, wf.summary);
        }
    }
    println!("\n`help <name>` shows the details of one command or workflow.");
}

fn print_command_help(c: &descriptor::CommandDesc) {
    println!("{}  ({})", c.pattern, c.kind);
    if !c.summary.is_empty() {
        println!("  {}", c.summary);
    }
    println!("\n  usage: iotsploit-host send '{}'", usage_line(c));
    if !c.params.is_empty() {
        println!();
        let w = c.params.iter().map(|p| p.name.len()).max().unwrap_or(0);
        for p in &c.params {
            let req = if p.required { "required" } else { "optional" };
            let pick = match (&p.options_fetch_query, &p.options_count_query) {
                (Some(f), Some(_)) => format!("  row index from {f}"),
                _ => String::new(),
            };
            println!("  {:<w$}  {:<6}  {req}{pick}", p.name, p.param_type);
        }
    }
    if let Some(r) = c.returns.as_deref().filter(|r| *r != "none") {
        println!("\n  returns: {r}");
    }
}

fn print_workflow_help(p: &Profile, w: &descriptor::WorkflowDesc) {
    if let Some(new) = &w.renamed_to {
        renamed(&w.name, new);
    }
    // An old name documents the current one.
    let name = w.renamed_to.as_deref().unwrap_or(&w.name);
    println!("{name}");
    if !w.summary.is_empty() {
        println!("  {}", w.summary);
    }
    // Workflow parameters are the trigger command's parameters, in order.
    let trigger = p.commands.iter().find(|c| scpi_matches(&c.pattern, header_of(&w.trigger_cmd)));
    let params: Vec<&descriptor::ParamDesc> = trigger.map(|c| c.params.iter().collect()).unwrap_or_default();
    let args: Vec<String> = params
        .iter()
        .map(|p| if p.required { format!("<{}>", p.name) } else { format!("[{}]", p.name) })
        .collect();
    println!("\n  usage: iotsploit-host workflow {}", [name.to_string()].into_iter().chain(args).collect::<Vec<_>>().join(" "));
    for prm in &params {
        let req = if prm.required { "required" } else { "optional" };
        println!("    {}  {}  {req}", prm.name, prm.param_type);
    }
    let cols = if w.fields.is_empty() { &w.result_fields } else { &w.fields };
    if !cols.is_empty() {
        let names: Vec<String> = cols
            .iter()
            .map(|c| match &c.unit {
                Some(u) => format!("{} ({u})", c.name),
                None => c.name.clone(),
            })
            .collect();
        println!("\n  results: {}", names.join(", "));
    }
    println!("  starts with {}; gives up after {} s", w.trigger_cmd, w.timeout_ms / 1000);
}

fn cmd_send(cli: &Cli) -> CliResult {
    if cli.args.is_empty() {
        return Err(usage("send needs a SCPI command, e.g. send '*IDN?'"));
    }
    // Unquoted arguments are joined, so `send GPIO:SET 2,1` works.
    let cmd = cli.args.join(" ");
    if scpi_matches("DATA:WRITE", header_of(&cmd)) {
        return Err(usage("send cannot carry a DATA:WRITE block"));
    }
    let (mut s, _) = open(cli)?;
    if is_error_query(&cmd) {
        println!("{}", s.query(&cmd)?);
        return Ok(());
    }
    let c = s.send_checked(&cmd)?;
    match &c.output {
        Output::Text(t) => println!("{t}"),
        Output::Block(b) => write_payload(b, cli.out.as_deref())?,
        Output::None if c.errors.is_empty() => eprintln!("ok"),
        Output::None => {}
    }
    if report_errors(&mut s, &cmd, &c) {
        return Err(Fail { code: EXIT_DEVICE, msg: String::new() });
    }
    Ok(())
}

fn cmd_workflow(cli: &Cli) -> CliResult {
    let name = cli
        .args
        .first()
        .ok_or_else(|| usage("workflow needs a name; `help` lists them"))?;
    let (mut s, _) = open(cli)?;
    let profile = read_descriptor(&mut s)?.ok_or_else(|| Fail {
        code: EXIT_DEVICE,
        msg: "this device does not describe workflows; update its firmware".into(),
    })?;
    match profile.workflow(name) {
        None => {
            let names: Vec<String> = profile.workflows.iter().map(|w| w.name.clone()).collect();
            return Err(not_found(name, "workflow", &names));
        }
        Some(w) => {
            if let Some(new) = &w.renamed_to {
                renamed(name, new);
            }
        }
    }
    iotsploit_host::workflow::run_workflow(&mut s, &profile, name, &cli.args[1..])?;
    Ok(())
}

fn cmd_errors(cli: &Cli) -> CliResult {
    let errs = open(cli)?.0.drain_errors()?;
    if errs.is_empty() {
        println!("(no errors)");
    }
    for e in errs {
        println!("{},\"{}\"", e.code, e.message);
    }
    Ok(())
}

fn cmd_repl(cli: &Cli) -> CliResult {
    let (mut s, address) = open(cli)?;
    println!("connected to {address}; type SCPI commands, `quit` or Ctrl-D to exit");
    let stdin = io::stdin();
    let mut line = String::new();
    loop {
        print!("> ");
        io::stdout().flush().ok();
        line.clear();
        if stdin.read_line(&mut line)? == 0 {
            println!();
            return Ok(());
        }
        let cmd = line.trim();
        if cmd.is_empty() {
            continue;
        }
        if cmd.eq_ignore_ascii_case("exit") || cmd.eq_ignore_ascii_case("quit") {
            return Ok(());
        }
        if is_error_query(cmd) {
            match s.query(cmd) {
                Ok(t) => println!("{t}"),
                Err(e) => eprintln!("error: {e}"),
            }
            continue;
        }
        match s.send_checked(cmd) {
            Ok(c) => {
                match &c.output {
                    Output::Text(t) => println!("{t}"),
                    Output::Block(b) => println!("(binary block, {} bytes)", b.len()),
                    Output::None if c.errors.is_empty() => println!("ok"),
                    Output::None => {}
                }
                report_errors(&mut s, cmd, &c);
            }
            Err(e) => eprintln!("error: {e}"),
        }
    }
}

#[cfg(feature = "tcp")]
fn cmd_stream(cli: &Cli) -> CliResult {
    use iotsploit_host::dataplane::{self, DataPlane, GapTracker};
    let limit: usize = cli.args.first().and_then(|a| a.parse().ok()).unwrap_or(0);

    // The control plane is the single source of truth for where the data
    // plane is and what it emits; nothing here is hardcoded.
    let (mut s, address) = open(cli)?;
    let host = match address.strip_prefix("tcp://") {
        Some(hp) => hp.rsplit_once(':').map_or(hp, |(h, _)| h).to_string(),
        None => return Err(usage("stream needs a network device: -d tcp://<host>")),
    };
    let (port, fmt) = match dataplane::discover(&mut s)? {
        Some(v) => v,
        None => {
            eprintln!("device has no data plane (SYSTem:STReam:PORT? returned 0)");
            return Ok(());
        }
    };
    eprintln!("stream: port {port}, v{} stride {}", fmt.version, fmt.stride);
    eprintln!("fields: {}", fmt.fields);

    let mut dp = DataPlane::connect((host.as_str(), port), fmt.stride, Duration::from_millis(cli.timeout_ms))?;
    s.write("SYSTem:STReam:STARt")?;

    // `dropped` is a u64 at a schema-declared offset. Locate it rather
    // than assuming, so a different source still reports gaps.
    let mut gaps = field_offset(&fmt.fields, "dropped").map(GapTracker::new);
    let mut n = 0usize;
    // Piping a record stream into `head` is normal usage, and Rust's
    // println! panics on EPIPE. Write explicitly and treat a closed
    // downstream as a reason to stop, not an error.
    let stdout = io::stdout();
    let mut out = stdout.lock();
    while let Some(rec) = dp.next_record()? {
        if let Some(g) = gaps.as_mut() {
            let lost = g.observe(&rec);
            if lost > 0 {
                eprintln!("-- {lost} record(s) lost before #{n} --");
            }
        }
        if writeln!(out, "{}", hex(&rec)).is_err() {
            break;
        }
        n += 1;
        if limit != 0 && n >= limit {
            break;
        }
    }
    let _ = s.write("SYSTem:STReam:STOP");
    eprintln!("{n} records, {} lost", gaps.as_ref().map(|g| g.total_lost()).unwrap_or(0));
    Ok(())
}

/// Byte offset of a named field in a `name:type[:unit],...` schema.
///
/// Only fixed-width scalar types are walked; anything else (a trailing
/// `bytesN` blob, say) ends the walk, which is fine because the counters this
/// is used for come first by convention.
#[cfg(feature = "tcp")]
fn field_offset(fields: &str, want: &str) -> Option<usize> {
    let mut off = 0usize;
    for field in fields.split(',') {
        let mut it = field.split(':');
        let name = it.next()?.trim();
        let size = match it.next()?.trim() {
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
    bytes.iter().map(|b| format!("{b:02x}")).collect()
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
    warn_caps(caps);
}

fn warn_caps(caps: &caps::Capabilities) {
    if !caps.parse_errors.is_empty() {
        let pairs: Vec<String> = caps.parse_errors.iter().map(|(k, v)| format!("{k}=\"{v}\"")).collect();
        eprintln!("warning: unparseable capability fields: {}", pairs.join(", "));
    }
}

fn main() -> ExitCode {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let result = parse_args(argv).map_err(usage).and_then(run);
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(f) => {
            if !f.msg.is_empty() {
                eprintln!("iotsploit-host: {}", f.msg);
            }
            if f.code == EXIT_USAGE {
                eprintln!("run `iotsploit-host --help` for usage");
            }
            ExitCode::from(f.code)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scpi_matching_accepts_long_and_short_forms() {
        assert!(scpi_matches("BLE:SCAN:STATe?", "ble:scan:stat?"));
        assert!(scpi_matches("BLE:SCAN:STATe?", "BLE:SCAN:STATE?"));
        assert!(scpi_matches("SYSTem:ERRor?", "SYST:ERR?"));
        assert!(!scpi_matches("BLE:SCAN:STATe?", "BLE:SCAN:STATe"));
        assert!(!scpi_matches("BLE:SCAN:STATe?", "BLE:SCAN:STA?"));
        assert!(!scpi_matches("GPIO:SET", "GPIO"));
    }

    #[test]
    fn suggests_the_closest_header() {
        let h: Vec<String> = ["BLE:SCAN:STATe?", "BLE:SCAN:COUNt?", "GPIO:SET"].iter().map(|s| s.to_string()).collect();
        assert_eq!(closest("BLE:SCAN:STAT", &h), Some("BLE:SCAN:STATe?"));
        assert_eq!(closest("GPIO:STE", &h), Some("GPIO:SET"));
        assert_eq!(closest("WLAN:CONNECT", &h), None);
    }

    #[test]
    fn options_are_accepted_anywhere() {
        let a = |v: &[&str]| v.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        let c = parse_args(a(&["send", "DATA:READ? 4", "-o", "x.bin", "-d", "2"])).ok().unwrap();
        assert_eq!(c.command, "send");
        assert_eq!(c.args, vec!["DATA:READ? 4"]);
        assert_eq!(c.out.as_deref(), Some("x.bin"));
        assert_eq!(c.device.as_deref(), Some("2"));
        let c = parse_args(a(&["--device=tcp://h", "info", "--timeout=100"])).ok().unwrap();
        assert_eq!(c.device.as_deref(), Some("tcp://h"));
        assert_eq!(c.timeout_ms, 100);
        assert!(parse_args(a(&["-d"])).is_err());
    }

    #[test]
    fn usage_line_marks_optional_parameters() {
        let c = descriptor::CommandDesc {
            pattern: "BLE:SCAN".into(),
            params: vec![descriptor::ParamDesc {
                name: "duration".into(),
                param_type: "u32".into(),
                ..Default::default()
            }],
            ..Default::default()
        };
        assert_eq!(usage_line(&c), "BLE:SCAN [duration]");
    }
}
