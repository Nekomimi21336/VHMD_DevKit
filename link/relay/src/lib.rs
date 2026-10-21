//! vhmd-link-relay: VHMD Link's data path between the headset app and the VHMD
//! driver, in native code (the PC app in Python only steers it).
//!
//! - Poses: from the headset as UDP datagrams (Wi-Fi) or TCP lines (USB, adb
//!   reverse) on port 39580, each a JSON array of VHMD messages, straight on to
//!   the driver (UDP 127.0.0.1:39570). The headset's own messages ("headset",
//!   "link_stats", "link_timing") go to the PC app instead.
//! - Back to the headset: the driver's haptic events, and what the PC app sends
//!   (its status).
//! - Discovery: answers "VHMD-DISCOVER" on UDP 39582.
//! - Video and sound: the headset connects to TCP 39581; the relay connects to
//!   the driver's encoder (127.0.0.1:39583), sends the settings line, and
//!   forwards the messages unchanged; the bitrate follows the network (lower
//!   when sending blocks, back up when it does not).
//! - Video over UDP (optional, "transport": "udp"): frames and sound go as UDP
//!   packets with parity (proto::udp) to the headset's "video_udp_port"; the
//!   TCP connection keeps the configuration. The headset's
//!   {"type":"link_request","keyframe":true} (a frame it could not rebuild)
//!   goes straight to the driver as a keyframe request.
//!
//! It runs as its own program or, preferably, inside the PC app's process as a
//! DLL (vhmdlinkrelay.dll, `vhmd_link_relay_start`): then the network ports
//! belong to the PC app, which the firewall already knows, and there is no
//! second process. Either way its threads are native: a busy or paused Python
//! never holds up poses or video.
//!
//! The PC app talks to it over UDP on 127.0.0.1 (the program prints the port on
//! its first line: "control 127.0.0.1:PORT"; the DLL returns it). Datagrams are
//! "KIND\n" + payload:
//!   app -> relay: HELLO (every second; the program exits 10 s after the last),
//!                 TO_HEADSET <json>, DRIVER <json> (sent from the pose socket,
//!                 so the driver keeps sending haptics there), VIDEO <json settings>
//!                 ({"bitrate_mbps","codec","buffer","encoder","eye_width","adaptive","restart"}),
//!                 QUIT (the program exits; the DLL stays until its process ends)
//!   relay -> app: MSG <headset message object>, REPLY <driver reply with "seq">,
//!                 STATS <json, every second>, LOG <text>
//!
//!   vhmd-link-relay [--listen 0.0.0.0|127.0.0.1] [--driver 127.0.0.1:39570]
//!                   [--control PORT] [--name PC-NAME] [--no-discovery]

use std::collections::VecDeque;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{IpAddr, Shutdown, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use vhmd_link_proto::*;

// -- settings and state ---------------------------------------------------------
#[derive(Clone)]
struct VideoSettings {
    bitrate: f64, // the most the adaptive control goes to
    codec: String,
    buffer: i64,
    encoder: String,
    eye_width: i64,
    adaptive: bool,
    udp: bool,      // frames and sound over UDP with parity
    udp_port: u16,  // the headset's port for them
    fec: u32,       // parity fragments per 100 data fragments
}

impl Default for VideoSettings {
    fn default() -> Self {
        VideoSettings {
            bitrate: 40.0,
            codec: "hevc".into(),
            buffer: 0,
            encoder: "auto".into(),
            eye_width: 0,
            adaptive: true,
            udp: false,
            udp_port: 0,
            fec: 20,
        }
    }
}

#[derive(Default)]
struct Stats {
    msgs: u64,
    bytes: u64,
    video_in: u64,
    video_out: u64,
    video_bytes: u64,
    relay_ms: Vec<f64>,
    forward_us: Vec<f64>,
    last: String,
    udp_packets: u64,
    keyframe_requests: u64,
}

enum Peer {
    None,
    Udp(SocketAddr),
    Tcp(TcpStream),
}

struct State {
    video: Mutex<VideoSettings>,
    bitrate_now: Mutex<f64>,
    stats: Mutex<Stats>,
    peer: Mutex<Peer>,
    last_seen: Mutex<Option<Instant>>,
    app: Mutex<Option<SocketAddr>>,
    app_seen: Mutex<Instant>,
    video_conn: Mutex<Option<TcpStream>>, // the headset's video connection
    driver_stream: Mutex<Option<TcpStream>>, // the driver's encoder connection
    quit: AtomicBool,
    started: Instant,
    haptics: AtomicU64,
    last_key_request: Mutex<Option<Instant>>,
}

struct Ctx {
    st: Arc<State>,
    control: UdpSocket,
    driver_udp: UdpSocket,
    driver: SocketAddr,
    pose_udp: Option<UdpSocket>,
}

impl Ctx {
    fn event(&self, kind: &str, payload: &str) {
        if let Some(app) = *self.st.app.lock().unwrap() {
            let mut b = Vec::with_capacity(kind.len() + 1 + payload.len());
            b.extend_from_slice(kind.as_bytes());
            b.push(b'\n');
            b.extend_from_slice(payload.as_bytes());
            let _ = self.control.send_to(&b, app);
        }
    }

    fn log(&self, text: &str) {
        self.event("LOG", text);
    }

    /// To the headset over whichever link it uses.
    fn to_headset(&self, data: &[u8]) {
        let mut peer = self.st.peer.lock().unwrap();
        match &mut *peer {
            Peer::Udp(addr) => {
                if let Some(s) = &self.pose_udp {
                    let _ = s.send_to(data, *addr);
                }
            }
            Peer::Tcp(t) => {
                let mut b = data.to_vec();
                b.push(b'\n');
                let _ = t.write_all(&b);
            }
            Peer::None => {}
        }
    }

    /// One JSON array from the headset: the headset's own messages to the app,
    /// the rest to the driver.
    fn forward(&self, data: &[u8]) {
        let t0 = Instant::now();
        *self.st.last_seen.lock().unwrap() = Some(t0);
        let text = match std::str::from_utf8(data) {
            Ok(t) => t,
            Err(_) => return,
        };
        let own = text.contains("\"type\":\"headset\"")
            || text.contains("\"type\":\"link_stats\"")
            || text.contains("\"type\":\"link_timing\"")
            || text.contains("\"type\":\"link_request\"");
        let out: std::borrow::Cow<str> = if own {
            let mut rest = String::with_capacity(text.len());
            rest.push('[');
            for obj in array_objects(text) {
                match message_type(obj).as_deref() {
                    Some("link_request") => {
                        // Lost a frame it could not rebuild: a keyframe, now (not via the app).
                        // At most one every 250 ms: a keyframe takes a moment to arrive.
                        let mut last = self.st.last_key_request.lock().unwrap();
                        let due = last.map_or(true, |t| t.elapsed() > Duration::from_millis(250));
                        if due {
                            *last = Some(Instant::now());
                        }
                        drop(last);
                        if due && json_bool(obj, "keyframe") == Some(true) {
                            if let Some(d) = self.st.driver_stream.lock().unwrap().as_mut() {
                                let _ = d.write_all(b"{\"keyframe\":true}\n");
                            }
                            self.st.stats.lock().unwrap().keyframe_requests += 1;
                        }
                    }
                    Some("headset") | Some("link_stats") | Some("link_timing") => self.event("MSG", obj),
                    _ => {
                        if rest.len() > 1 {
                            rest.push(',');
                        }
                        rest.push_str(obj);
                    }
                }
            }
            rest.push(']');
            rest.into()
        } else {
            text.into()
        };
        let _ = self.driver_udp.send_to(out.as_bytes(), self.driver);
        let us = t0.elapsed().as_secs_f64() * 1e6;
        let mut s = self.st.stats.lock().unwrap();
        s.msgs += 1;
        s.bytes += out.len() as u64;
        s.forward_us.push(us);
        if s.msgs % 16 == 1 && out.len() < 4096 {   // a sample for the app's display
            s.last.clear();
            s.last.push_str(&out);
        }
    }
}

// -- socket options -------------------------------------------------------------
fn set_sndbuf(s: &TcpStream, bytes: i32) {
    set_socket_buffer(s, true, bytes);
}

// -- start ------------------------------------------------------------------------
/// What to start with (the program's command line, or the DLL's argument).
pub struct Config {
    pub listen: IpAddr,
    pub driver: SocketAddr,
    pub control_port: u16,
    pub name: String,
    pub discovery: bool,
    pub embedded: bool, // inside another process: never exit it
}

impl Config {
    /// From command-line style arguments.
    pub fn from_args(args: &[String], embedded: bool) -> Config {
        let arg = |name: &str| args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).cloned();
        let listen: IpAddr = arg("--listen").and_then(|s| s.parse().ok()).unwrap_or(IpAddr::from([0, 0, 0, 0]));
        Config {
            listen,
            driver: arg("--driver").and_then(|s| s.parse().ok()).unwrap_or(SocketAddr::from(([127, 0, 0, 1], DRIVER_PORT))),
            control_port: arg("--control").and_then(|s| s.parse().ok()).unwrap_or(0),
            name: arg("--name").or_else(|| std::env::var("COMPUTERNAME").ok()).unwrap_or_else(|| "PC".into()),
            discovery: !args.iter().any(|a| a == "--no-discovery") && !listen.is_loopback(),
            embedded,
        }
    }
}

pub fn usage() -> String {
    include_str!("lib.rs").lines().take_while(|l| l.starts_with("//!")).map(|l| l.trim_start_matches("//!").strip_prefix(' ').unwrap_or("")).collect::<Vec<_>>().join("\n")
}

/// Binds the ports and starts the threads; the control port, or why not
/// (a port in use: VHMD Link is probably running already).
pub fn start(cfg: Config) -> Result<u16, String> {
    let Config { listen, driver, control_port, name, discovery, embedded } = cfg;
    let busy = |what: String, e: std::io::Error| format!("{what}: {e} (is VHMD Link already running?)");
    let control = UdpSocket::bind(("127.0.0.1", control_port)).map_err(|e| busy("control socket".into(), e))?;
    let port = control.local_addr().map_err(|e| e.to_string())?.port();
    let driver_udp = UdpSocket::bind("127.0.0.1:0").map_err(|e| busy("driver socket".into(), e))?;
    let pose_udp = if listen.is_loopback() {
        None
    } else {
        Some(UdpSocket::bind((listen, POSE_PORT)).map_err(|e| busy(format!("UDP {POSE_PORT}"), e))?)
    };
    let pose_tcp = TcpListener::bind((listen, POSE_PORT)).map_err(|e| busy(format!("TCP {POSE_PORT}"), e))?;
    let video = TcpListener::bind((listen, VIDEO_PORT)).map_err(|e| busy(format!("TCP {VIDEO_PORT}"), e))?;

    let st = Arc::new(State {
        video: Mutex::new(VideoSettings::default()),
        bitrate_now: Mutex::new(40.0),
        stats: Mutex::new(Stats::default()),
        peer: Mutex::new(Peer::None),
        last_seen: Mutex::new(None),
        app: Mutex::new(None),
        app_seen: Mutex::new(Instant::now()),
        video_conn: Mutex::new(None),
        driver_stream: Mutex::new(None),
        quit: AtomicBool::new(false),
        started: Instant::now(),
        haptics: AtomicU64::new(0),
        last_key_request: Mutex::new(None),
    });
    let ctx = Arc::new(Ctx {
        st: st.clone(),
        control: control.try_clone().unwrap(),
        driver_udp: driver_udp.try_clone().unwrap(),
        driver,
        pose_udp: pose_udp.as_ref().map(|s| s.try_clone().unwrap()),
    });

    // Poses over the network.
    if let Some(s) = pose_udp {
        let c = ctx.clone();
        thread::spawn(move || {
            let mut buf = vec![0u8; 65536];
            loop {
                match s.recv_from(&mut buf) {
                    Ok((n, from)) => {
                        {
                            let mut p = c.st.peer.lock().unwrap();
                            if !matches!(&*p, Peer::Udp(a) if *a == from) {
                                *p = Peer::Udp(from);
                                drop(p);
                                c.log(&format!("headset sending over the network from {}", from.ip()));
                            }
                        }
                        c.forward(&buf[..n]);
                    }
                    Err(_) => continue, // Windows: ICMP port unreachable from an earlier send
                }
            }
        });
    }
    // Poses over USB (adb reverse): one array per line.
    {
        let c = ctx.clone();
        thread::spawn(move || {
            for conn in pose_tcp.incoming().flatten() {
                conn.set_nodelay(true).ok();
                c.log(&format!("headset connected over USB {}", conn.peer_addr().map(|a| a.to_string()).unwrap_or_default()));
                if let Ok(w) = conn.try_clone() {
                    *c.st.peer.lock().unwrap() = Peer::Tcp(w);
                }
                let c2 = c.clone();
                thread::spawn(move || {
                    let mut r = BufReader::with_capacity(65536, conn);
                    let mut line = Vec::new();
                    loop {
                        line.clear();
                        match r.read_until(b'\n', &mut line) {
                            Ok(0) | Err(_) => break,
                            Ok(_) => {
                                while line.last().is_some_and(|b| *b == b'\n' || *b == b'\r') {
                                    line.pop();
                                }
                                if !line.is_empty() {
                                    c2.forward(&line);
                                }
                            }
                        }
                    }
                });
            }
        });
    }
    // Discovery.
    if discovery {
        let reply = format!("{{\"vhmd\":\"pc\",\"name\":{},\"pose_port\":{POSE_PORT},\"video_port\":{VIDEO_PORT}}}", json_quote(&name));
        match UdpSocket::bind((listen, DISCOVERY_PORT)) {
            Ok(s) => {
                thread::spawn(move || {
                    let mut buf = [0u8; 1024];
                    loop {
                        if let Ok((n, from)) = s.recv_from(&mut buf) {
                            if buf[..n].starts_with(b"VHMD-DISCOVER") {
                                let _ = s.send_to(reply.as_bytes(), from);
                            }
                        }
                    }
                });
            }
            Err(e) => ctx.log(&format!("discovery port {DISCOVERY_PORT}: {e}")),
        }
    }
    // The driver's replies: haptics to the headset, answers to the app.
    {
        let c = ctx.clone();
        thread::spawn(move || {
            let mut buf = vec![0u8; 65536];
            loop {
                if let Ok((n, _)) = driver_udp.recv_from(&mut buf) {
                    let d = &buf[..n];
                    if contains(d, b"\"haptic\"") {
                        c.st.haptics.fetch_add(1, Ordering::Relaxed);
                        c.to_headset(d);
                    } else if contains(d, b"\"seq\"") {
                        c.event("REPLY", &String::from_utf8_lossy(d));
                    }
                }
            }
        });
    }
    // Video.
    {
        let c = ctx.clone();
        thread::spawn(move || {
            for conn in video.incoming().flatten() {
                let peer = conn.peer_addr().map(|a| a.ip().to_string()).unwrap_or_default();
                c.log(&format!("video: headset connected from {peer}"));
                if let Ok(k) = conn.try_clone() {
                    *c.st.video_conn.lock().unwrap() = Some(k);
                }
                if let Err(e) = relay_video(&c, conn) {
                    c.log(&format!("video: stopped ({e})"));
                }
                *c.st.video_conn.lock().unwrap() = None;
                *c.st.driver_stream.lock().unwrap() = None;
            }
        });
    }
    // Statistics for the app, every second; exit when the app is gone.
    {
        let c = ctx.clone();
        thread::spawn(move || loop {
            thread::sleep(Duration::from_secs(1));
            if !embedded && (c.st.app_seen.lock().unwrap().elapsed() > Duration::from_secs(10) || c.st.quit.load(Ordering::Relaxed)) {
                std::process::exit(0);
            }
            let stats = {
                let mut s = c.st.stats.lock().unwrap();
                std::mem::take(&mut *s)
            };
            let (ra, rp, rm) = summary(&stats.relay_ms);
            let (fa, _fp, fm) = summary(&stats.forward_us);
            let link = match &*c.st.peer.lock().unwrap() {
                Peer::Udp(a) => format!("\"wifi\",\"address\":{}", json_quote(&a.ip().to_string())),
                Peer::Tcp(_) => "\"usb\",\"address\":\"USB\"".to_string(),
                Peer::None => "null,\"address\":\"\"".to_string(),
            };
            let seen = c.st.last_seen.lock().unwrap().map(|t| t.elapsed().as_secs_f64()).unwrap_or(1e9);
            let json = format!(
                "{{\"msgs\":{},\"bytes\":{},\"video_in\":{},\"video_out\":{},\"video_bytes\":{},\"relay_ms\":[{ra:.3},{rp:.3},{rm:.3}],\
                 \"forward_us\":[{fa:.1},{fm:.1}],\"link\":{link},\"last_seen_s\":{seen:.2},\"video_connected\":{},\"bitrate_now\":{:.1},\
                 \"haptics\":{},\"uptime_s\":{:.0},\"udp_packets\":{},\"keyframe_requests\":{},\"last\":{}}}",
                stats.msgs,
                stats.bytes,
                stats.video_in,
                stats.video_out,
                stats.video_bytes,
                c.st.video_conn.lock().unwrap().is_some(),
                *c.st.bitrate_now.lock().unwrap(),
                c.st.haptics.load(Ordering::Relaxed),
                c.st.started.elapsed().as_secs_f64(),
                stats.udp_packets,
                stats.keyframe_requests,
                json_quote(&stats.last),
            );
            c.event("STATS", &json);
        });
    }
    // Commands from the app.
    thread::spawn(move || {
    let mut buf = vec![0u8; 65536];
    loop {
        let Ok((n, from)) = control.recv_from(&mut buf) else { continue };
        if !from.ip().is_loopback() {
            continue;
        }
        *st.app.lock().unwrap() = Some(from);
        *st.app_seen.lock().unwrap() = Instant::now();
        let d = &buf[..n];
        let (kind, payload) = match d.iter().position(|b| *b == b'\n') {
            Some(i) => (&d[..i], &d[i + 1..]),
            None => (d, &d[d.len()..]),
        };
        match kind {
            b"HELLO" => {}
            b"QUIT" => {
                if !embedded {
                    std::process::exit(0)
                }
            }
            b"TO_HEADSET" => ctx.to_headset(payload),
            b"DRIVER" => {
                let _ = ctx.driver_udp.send_to(payload, ctx.driver);
            }
            b"VIDEO" => {
                let p = String::from_utf8_lossy(payload).into_owned();
                apply_video_settings(&ctx, &p);
            }
            _ => {}
        }
    }
    });
    Ok(port)
}

// -- DLL ------------------------------------------------------------------------------
/// Starts the relay inside the calling process. args: command-line style
/// arguments, one per line ("--listen\n0.0.0.0\n--name\nPC"). Returns the
/// control port, or 0 with the reason in err (UTF-8, NUL-terminated).
#[no_mangle]
pub extern "C" fn vhmd_link_relay_start(args: *const std::os::raw::c_char, err: *mut u8, err_len: usize) -> u16 {
    let text = if args.is_null() {
        String::new()
    } else {
        unsafe { std::ffi::CStr::from_ptr(args) }.to_string_lossy().into_owned()
    };
    let list: Vec<String> = text.lines().map(|l| l.to_string()).filter(|l| !l.is_empty()).collect();
    match start(Config::from_args(&list, true)) {
        Ok(port) => port,
        Err(e) => {
            if !err.is_null() && err_len > 0 {
                let b = e.as_bytes();
                let n = b.len().min(err_len - 1);
                unsafe {
                    std::ptr::copy_nonoverlapping(b.as_ptr(), err, n);
                    *err.add(n) = 0;
                }
            }
            0
        }
    }
}

fn contains(hay: &[u8], needle: &[u8]) -> bool {
    hay.windows(needle.len()).any(|w| w == needle)
}

fn summary(v: &[f64]) -> (f64, f64, f64) {
    if v.is_empty() {
        return (0.0, 0.0, 0.0);
    }
    let mut s = v.to_vec();
    s.sort_by(|a, b| a.partial_cmp(b).unwrap());
    (s.iter().sum::<f64>() / s.len() as f64, s[(s.len() * 99 / 100).min(s.len() - 1)], s[s.len() - 1])
}

fn apply_video_settings(ctx: &Ctx, p: &str) {
    let mut v = ctx.st.video.lock().unwrap();
    let old = v.clone();
    if let Some(b) = json_number(p, "bitrate_mbps") {
        v.bitrate = b.clamp(1.0, 500.0);
    }
    if let Some(c) = json_string(p, "codec") {
        v.codec = c;
    }
    if let Some(b) = json_number(p, "buffer") {
        v.buffer = b as i64;
    }
    if let Some(e) = json_string(p, "encoder") {
        v.encoder = e;
    }
    if let Some(w) = json_number(p, "eye_width") {
        v.eye_width = w as i64;
    }
    if let Some(a) = json_bool(p, "adaptive") {
        v.adaptive = a;
    }
    if let Some(t) = json_string(p, "transport") {
        v.udp = t == "udp";
    }
    if let Some(port) = json_number(p, "udp_port") {
        v.udp_port = port as u16;
    }
    if let Some(f) = json_number(p, "fec") {
        v.fec = (f as u32).min(100);
    }
    if json_bool(p, "keyframe") == Some(true) {
        if let Some(d) = ctx.st.driver_stream.lock().unwrap().as_mut() {
            let _ = d.write_all(b"{\"keyframe\":true}\n");
        }
    }
    let restart = json_bool(p, "restart").unwrap_or(false)
        || v.codec != old.codec
        || v.buffer != old.buffer
        || v.encoder != old.encoder
        || v.eye_width != old.eye_width
        || v.udp != old.udp
        || (v.udp && (v.udp_port != old.udp_port || v.fec != old.fec));
    let bitrate_changed = (v.bitrate - old.bitrate).abs() > 0.01;
    let new_bitrate = v.bitrate;
    drop(v);
    if restart {
        // The headset reconnects (about a second) and gets the new settings.
        if let Some(c) = ctx.st.video_conn.lock().unwrap().as_ref() {
            let _ = c.shutdown(Shutdown::Both);
        }
    } else if bitrate_changed {
        let mut now = ctx.st.bitrate_now.lock().unwrap();
        *now = now.min(new_bitrate).max(1.0);
        if new_bitrate > *now && !ctx.st.video.lock().unwrap().adaptive {
            *now = new_bitrate;
        }
        let line = format!("{{\"bitrate_mbps\":{:.1}}}\n", *now);
        if let Some(d) = ctx.st.driver_stream.lock().unwrap().as_mut() {
            let _ = d.write_all(line.as_bytes());
        }
    }
}

/// One headset video connection: from the driver's encoder, as it comes.
fn relay_video(c: &Ctx, mut conn: TcpStream) -> std::io::Result<()> {
    let mut drv = match TcpStream::connect_timeout(&SocketAddr::from(([127, 0, 0, 1], DRIVER_STREAM_PORT)), Duration::from_secs(1)) {
        Ok(d) => d,
        Err(_) => {
            // SteamVR not (yet) running: the headset comes back in a second.
            return Err(std::io::Error::other("waiting for SteamVR (the driver's encoder is not there)"));
        }
    };
    let v = c.st.video.lock().unwrap().clone();
    *c.st.bitrate_now.lock().unwrap() = v.bitrate;
    conn.set_nodelay(true)?;
    drv.set_nodelay(true)?;
    // About 60 ms of video in the send buffer: when the network is slower than
    // the video, sending blocks soon (the signal to lower the bitrate).
    set_sndbuf(&conn, ((v.bitrate * 1e6 * 0.06 / 8.0) as i32).max(256 << 10));
    let settings = format!(
        "{{\"bitrate_mbps\":{:.1},\"codec\":{},\"eye_width\":{},\"buffer\":{},\"encoder\":{}}}\n",
        v.bitrate,
        json_quote(&v.codec),
        v.eye_width,
        v.buffer,
        json_quote(&v.encoder)
    );
    drv.write_all(settings.as_bytes())?;
    *c.st.driver_stream.lock().unwrap() = Some(drv.try_clone()?);
    c.log(&format!("video: from the driver's encoder ({})", settings.trim()));
    // Frames and sound over UDP (with parity) when the headset asked for it.
    let udp = if v.udp && v.udp_port != 0 {
        let to = SocketAddr::new(conn.peer_addr()?.ip(), v.udp_port);
        let sock = UdpSocket::bind(SocketAddr::new(conn.local_addr()?.ip(), 0))?;
        set_socket_buffer(&sock, true, 8 << 20);
        c.log(&format!("video: frames over UDP to {to} with {}% parity", v.fec));
        Some((sock, to, udp::Packetizer::new(udp::DEFAULT_FRAG, v.fec)))
    } else {
        None
    };
    let mut udp = udp;

    let mut reader = BufReader::with_capacity(1 << 20, drv);
    let mut msg: Vec<u8> = Vec::with_capacity(1 << 20);
    let mut window = Instant::now();
    let mut blocked = 0f64;
    let (mut calm, mut hold_until, mut ceiling, mut ceiling_at) = (0u32, Instant::now(), 0f64, Instant::now() - Duration::from_secs(60));
    let mut delays: VecDeque<f64> = VecDeque::new();
    loop {
        msg.resize(HEADER_SIZE, 0);
        reader.read_exact(&mut msg[..HEADER_SIZE])?;
        let hb: &[u8; HEADER_SIZE] = msg[..HEADER_SIZE].try_into().unwrap();
        let h = Header::parse(hb).ok_or_else(|| std::io::Error::other("bad stream framing from the driver"))?;
        msg.resize(HEADER_SIZE + h.size as usize, 0);
        reader.read_exact(&mut msg[HEADER_SIZE..])?;
        if h.kind == KIND_CONFIG {
            c.log(&format!("video: {}", String::from_utf8_lossy(&msg[HEADER_SIZE..])));
        }
        let t = Instant::now();
        match udp.as_mut() {
            Some((sock, to, pk)) if h.kind == KIND_FRAME || h.kind == KIND_AUDIO => {
                // Keyframes matter most (everything after needs them): twice the parity.
                let fec = if h.flags & 2 != 0 { (pk.fec * 2).clamp(40, 100) } else { pk.fec };
                let packets = pk.packets_with(&msg, fec, h.kind as u8);
                c.st.stats.lock().unwrap().udp_packets += packets.len() as u64;
                for p in packets {
                    let _ = sock.send_to(&p, *to);
                }
            }
            _ => conn.write_all(&msg)?,
        }
        let now = Instant::now();
        blocked += (now - t).as_secs_f64();
        if h.kind == KIND_FRAME {
            let delay = (now_us() as f64 - h.time_us as f64) / 1000.0;
            delays.push_back(delay);
            let mut s = c.st.stats.lock().unwrap();
            s.video_in += 1;
            s.video_out += 1;
            s.video_bytes += h.size as u64;
        }
        let dt = (now - window).as_secs_f64();
        if dt >= 1.0 {
            {
                let mut s = c.st.stats.lock().unwrap();
                s.relay_ms.extend(delays.drain(..));
            }
            let busy = blocked / dt;
            blocked = 0.0;
            window = now;
            let v = c.st.video.lock().unwrap().clone();
            let mut cur = *c.st.bitrate_now.lock().unwrap();
            let mut new = cur;
            if busy > 0.2 {
                ceiling = cur;
                ceiling_at = now;
                new = (cur * 0.7).max(8.0);
                calm = 0;
                hold_until = now + Duration::from_secs(5);
            } else if busy < 0.03 {
                calm += 1;
                let limit = if now - ceiling_at > Duration::from_secs(30) { v.bitrate } else { v.bitrate.min(ceiling * 0.85) };
                if calm >= 3 && now >= hold_until && cur < limit - 0.5 {
                    new = limit.min(cur * 1.15);
                    calm = 0;
                    hold_until = now + Duration::from_secs(2);
                }
            } else {
                calm = 0;
            }
            if v.adaptive && (new - cur).abs() >= 0.5 {
                c.log(&format!(
                    "video: {} the bitrate to {:.0} Mbit/s (sending was blocked {:.0}% of the last second)",
                    if new < cur { "lowering" } else { "raising" },
                    new,
                    busy * 100.0
                ));
                cur = new;
                *c.st.bitrate_now.lock().unwrap() = cur;
                let line = format!("{{\"bitrate_mbps\":{cur:.1}}}\n");
                if let Some(d) = c.st.driver_stream.lock().unwrap().as_mut() {
                    d.write_all(line.as_bytes())?;
                }
            }
        }
    }
}
