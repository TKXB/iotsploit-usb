//! `iotsploit-host` CLI binary.
//!
//! A small, dependency-free command line for talking to `iotsploit-usb`
//! devices over the Linux kernel USBTMC driver.

use std::io::{self, Write};
use std::path::PathBuf;
use std::process::ExitCode;

use iotsploit_host::{
    caps, headers, session::ScpiSession, usbtmc_kernel::UsbtmcKernel, Error,
};

const HELP: &str = "\
iotsploit-host - generic SCPI-over-USBTMC host for iotsploit-usb

USAGE:
    iotsploit-host [--device <path>] <command> [args]

GLOBAL OPTIONS:
    -d, --device <path>   /dev/usbtmcN node (default: auto-detect single node)
    -h, --help            Show this help
    -V, --version         Show version

COMMANDS:
    list                         List /dev/usbtmc* device nodes
    idn                          Query *IDN?
    caps                         Query and parse SYSTem:CAPabilities?
    headers                      List command headers (SYSTem:HELP:HEADers?)
    query  <cmd>                 Send a SCPI query and print the text response
    write  <cmd>                 Send a SCPI command (no response printed)
    block-read <cmd> [--out F]   Query an arbitrary-block response; write the
                                 payload to stdout or file F
    errors                       Drain the SYSTem:ERRor? queue
    repl                         Interactive SCPI prompt (Ctrl-D to exit)

EXAMPLES:
    iotsploit-host idn
    iotsploit-host caps
    iotsploit-host headers
    iotsploit-host query '*IDN?'
    iotsploit-host write 'GPIO:SET 2,1'
    iotsploit-host block-read 'DATA:READ? 64' --out adc.bin
    iotsploit-host repl
";

struct Cli {
    device: Option<PathBuf>,
    command: String,
    args: Vec<String>,
}

fn parse_args(argv: Vec<String>) -> Result<Cli, String> {
    let mut device: Option<PathBuf> = None;
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
            "-d" | "--device" => {
                i += 1;
                let v = argv
                    .get(i)
                    .ok_or_else(|| "missing value for --device".to_string())?;
                device = Some(PathBuf::from(v));
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
        device,
        command,
        args: positionals[1..].to_vec(),
    })
}

fn open_session(device: Option<PathBuf>) -> Result<ScpiSession<UsbtmcKernel>, Error> {
    let transport = match device {
        Some(p) => UsbtmcKernel::open(&p)?,
        None => UsbtmcKernel::auto_detect()?,
    };
    // Honour the device max-block when known; keep a generous read size.
    Ok(ScpiSession::new(transport).with_read_size(8192).with_max_block_len(1 << 20))
}

fn run(cli: Cli) -> Result<(), Error> {
    match cli.command.as_str() {
        "list" => {
            for node in list_nodes() {
                println!("{}", node.display());
            }
            Ok(())
        }
        "idn" => {
            let mut s = open_session(cli.device)?;
            let idn = s.idn()?;
            println!("{idn}");
            Ok(())
        }
        "caps" => {
            let mut s = open_session(cli.device)?;
            let caps = s.caps()?;
            print_caps(&caps);
            Ok(())
        }
        "headers" => {
            let mut s = open_session(cli.device)?;
            let h = headers::fetch_headers(&mut s, None)?;
            for line in h {
                println!("{line}");
            }
            Ok(())
        }
        "query" => {
            let cmd = cli
                .args
                .first()
                .ok_or_else(|| Error::Device("query requires a command argument".into()))?;
            let mut s = open_session(cli.device)?;
            let resp = s.query(cmd)?;
            println!("{resp}");
            Ok(())
        }
        "write" => {
            let cmd = cli
                .args
                .first()
                .ok_or_else(|| Error::Device("write requires a command argument".into()))?;
            let mut s = open_session(cli.device)?;
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
            let mut s = open_session(cli.device)?;
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
            let mut s = open_session(cli.device)?;
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
        "repl" => repl(cli.device),
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

fn repl(device: Option<PathBuf>) -> Result<(), Error> {
    let mut s = open_session(device)?;
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
