//! vhmd-link-relay as its own program (see lib.rs; the PC app prefers the DLL).

use std::io::Write;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "-h" || a == "--help") {
        println!("{}", vhmdlinkrelay::usage());
        return;
    }
    match vhmdlinkrelay::start(vhmdlinkrelay::Config::from_args(&args, false)) {
        Ok(port) => {
            println!("control 127.0.0.1:{port}");
            let _ = std::io::stdout().flush();
            loop {
                std::thread::park();
            }
        }
        Err(e) => {
            eprintln!("vhmd-link-relay: {e}");
            std::process::exit(3);
        }
    }
}
