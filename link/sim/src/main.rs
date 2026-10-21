//! vhmd-link-sim: plays the VHMD Link headset app on the PC, so the whole link
//! (bridge or relay, driver, SteamVR, encoder) can be tested without a headset.
//!
//! Like the app it finds the PC, sends head / hand poses every display frame
//! (as UDP datagrams, or TCP lines with --usb), its display description, link
//! statistics and frame timing (for the PC's display sync), and receives the
//! video and the sound. A simulated 72 Hz display takes the newest frame at
//! each refresh (as the app's "lowest latency" mode), so shown / repeated /
//! dropped frames and the wait before display are counted the same way.
//! Decoding is not simulated (instant); --check decodes the saved stream with
//! ffmpeg at the end.
//!
//! Measured on one clock (this PC):
//! - transit: driver sent the frame -> it arrived here (relay + network),
//! - pose to frame: a pose sent here -> the frame rendered with it arrived
//!   (bridge / relay, SteamVR, render, encode, transit),
//! - look-ahead: like the app, when the PC asks for it ("predict").
//!
//!   vhmd-link-sim [--pc ADDR] [--usb] [--seconds N] [--rate HZ]
//!                 [--motion turn|still] [--save FILE] [--check] [--snapshot FILE.png]
//!                 [--edition NAME] [--no-info] [--json FILE] [--quiet]

use std::collections::VecDeque;
use std::f64::consts::PI;
use std::fs::File;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpStream, UdpSocket};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use vhmd_link_proto::*;

// -- options ----------------------------------------------------------------------
struct Opts {
    pc: Option<IpAddr>,
    usb: bool,
    seconds: f64,
    rate: f64,
    motion: String,
    save: Option<String>,
    check: bool,
    snapshot: Option<String>,
    edition: String,
    info: bool,
    json: Option<String>,
    quiet: bool,
}

fn parse_args() -> Opts {
    let mut o = Opts {
        pc: None,
        usb: false,
        seconds: 30.0,
        rate: 72.0,
        motion: "turn".into(),
        save: None,
        check: false,
        snapshot: None,
        edition: "sim".into(),
        info: true,
        json: None,
        quiet: false,
    };
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    let val = |i: &mut usize| -> String {
        *i += 1;
        args.get(*i).cloned().unwrap_or_else(|| {
            eprintln!("missing value for {}", args[*i - 1]);
            std::process::exit(2)
        })
    };
    while i < args.len() {
        match args[i].as_str() {
            "--pc" => o.pc = Some(val(&mut i).parse().expect("--pc: an IP address")),
            "--usb" => o.usb = true,
            "--seconds" => o.seconds = val(&mut i).parse().expect("--seconds"),
            "--rate" => o.rate = val(&mut i).parse().expect("--rate"),
            "--motion" => o.motion = val(&mut i),
            "--save" => o.save = Some(val(&mut i)),
            "--check" => o.check = true,
            "--snapshot" => o.snapshot = Some(val(&mut i)),
            "--edition" => o.edition = val(&mut i),
            "--no-info" => o.info = false,
            "--json" => o.json = Some(val(&mut i)),
            "--quiet" => o.quiet = true,
            "-h" | "--help" => {
                println!("{}", include_str!("main.rs").lines().take_while(|l| l.starts_with("//!")).map(|l| l.trim_start_matches("//!").strip_prefix(' ').unwrap_or("")).collect::<Vec<_>>().join("\n"));
                std::process::exit(0);
            }
            a => {
                eprintln!("unknown argument {a} (--help)");
                std::process::exit(2);
            }
        }
        i += 1;
    }
    o
}

// -- poses ------------------------------------------------------------------------
#[derive(Clone, Copy, Debug, Default)]
struct Pose {
    p: [f64; 3],
    q: [f64; 4], // w x y z
}

/// Head pose at time t (seconds): turning +-30 degrees at 0.5 Hz, swaying a little.
fn head_at(motion: &str, t: f64) -> Pose {
    if motion == "still" {
        return Pose { p: [0.0, 1.65, 0.0], q: [1.0, 0.0, 0.0, 0.0] };
    }
    let yaw = (30.0f64).to_radians() * (2.0 * PI * 0.5 * t).sin();
    let pitch = (8.0f64).to_radians() * (2.0 * PI * 0.31 * t).sin();
    let (cy, sy) = ((yaw / 2.0).cos(), (yaw / 2.0).sin());
    let (cp, sp) = ((pitch / 2.0).cos(), (pitch / 2.0).sin());
    // yaw about Y, then pitch about X
    let q = [cy * cp, cy * sp, sy * cp, -sy * sp];
    Pose { p: [0.05 * (2.0 * PI * 0.3 * t).sin(), 1.65, 0.02 * (2.0 * PI * 0.2 * t).sin()], q }
}

fn angle_between(a: &[f64; 4], b: &[f64; 4]) -> f64 {
    let d = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]).abs().min(1.0);
    2.0 * d.acos()
}

fn pose_json(id: &str, p: &Pose) -> String {
    format!(
        "{{\"type\":\"pose\",\"id\":\"{id}\",\"pos\":[{:.5},{:.5},{:.5}],\"rot\":[{:.6},{:.6},{:.6},{:.6}],\"src\":\"manual\"}}",
        p.p[0], p.p[1], p.p[2], p.q[0], p.q[1], p.q[2], p.q[3]
    )
}

struct Sent {
    sent_us: u64,     // wall clock, when sent
    pred_s: f64,      // the time (seconds since start) the pose was predicted for
    ahead_ms: f64,    // look-ahead it included
    pose: Pose,
}

// -- shared state -----------------------------------------------------------------
struct Arrived {
    index: u64,
    arrived: Instant,
    arrived_us: u64,
    pose: Option<Pose>,
}

#[derive(Default)]
struct Window {
    v: Vec<f64>,
}
impl Window {
    fn add(&mut self, x: f64) {
        self.v.push(x);
    }
    fn take(&mut self) -> (f64, f64, f64, usize) {
        // avg, p95, max, n
        if self.v.is_empty() {
            return (0.0, 0.0, 0.0, 0);
        }
        let mut v = std::mem::take(&mut self.v);
        v.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let n = v.len();
        (v.iter().sum::<f64>() / n as f64, v[(n * 95 / 100).min(n - 1)], v[n - 1], n)
    }
}

#[derive(Default)]
struct Totals {
    frames: u64,
    bytes: u64,
    lost: u64,
    out_of_order: u64,
    configs: u64,
    audio_packets: u64,
    audio_format: String,
    codec: String,
    width: u64,
    height: u64,
    shown: u64,
    repeats: u64,
    dropped: u64,
    haptics: u64,
    status: String,
    transit_sum: f64,
    transit_max: f64,
    pose_lat_sum: f64,
    pose_lat_n: u64,
    pose_lat_max: f64,
    wait_sum: f64,
}

#[derive(Default)]
struct Shared {
    queue: VecDeque<Arrived>,
    totals: Totals,
    // per-second windows
    w_frames: u64,
    w_bytes: u64,
    w_audio: u64,
    transit: Window,
    pose_lat: Window,
    connected: bool,
    predict: bool,
    sent: VecDeque<Sent>,
}

// -- finding the PC ---------------------------------------------------------------
fn discover() -> Option<IpAddr> {
    let s = UdpSocket::bind("0.0.0.0:0").ok()?;
    s.set_broadcast(true).ok()?;
    s.set_read_timeout(Some(Duration::from_millis(700))).ok()?;
    s.send_to(b"VHMD-DISCOVER 1", (Ipv4Addr::BROADCAST, DISCOVERY_PORT)).ok()?;
    let mut buf = [0u8; 512];
    let (n, from) = s.recv_from(&mut buf).ok()?;
    let text = String::from_utf8_lossy(&buf[..n]);
    text.contains("\"vhmd\"").then_some(from.ip())
}

// -- main -------------------------------------------------------------------------
fn main() {
    let o = parse_args();
    let pc = o.pc.or_else(discover).unwrap_or(IpAddr::V4(Ipv4Addr::LOCALHOST));
    let found_by = if o.pc.is_some() { "given" } else if pc.is_loopback() { "fallback" } else { "discovery" };
    if !o.quiet {
        println!("vhmd-link-sim: PC {pc} ({found_by}), poses over {}, {} Hz, motion {}", if o.usb { "TCP (USB style)" } else { "UDP (Wi-Fi style)" }, o.rate, o.motion);
    }
    let shared = Arc::new(Mutex::new(Shared::default()));
    let stop = Arc::new(AtomicBool::new(false));
    let start = Instant::now();
    let start_us = now_us();

    // Pose link and the messages coming back on it.
    let udp = UdpSocket::bind("0.0.0.0:0").expect("udp socket");
    let mut tcp: Option<TcpStream> = None;
    if o.usb {
        let t = TcpStream::connect((pc, POSE_PORT)).expect("cannot connect to the PC's pose port (TCP)");
        t.set_nodelay(true).ok();
        let reader = t.try_clone().unwrap();
        let (sh, st) = (shared.clone(), stop.clone());
        thread::spawn(move || {
            for line in BufReader::new(reader).lines() {
                if st.load(Ordering::Relaxed) {
                    break;
                }
                match line {
                    Ok(l) => back_message(&sh, &l),
                    Err(_) => break,
                }
            }
        });
        tcp = Some(t);
    } else {
        let r = udp.try_clone().unwrap();
        r.set_read_timeout(Some(Duration::from_millis(200))).ok();
        let (sh, st) = (shared.clone(), stop.clone());
        thread::spawn(move || {
            let mut buf = vec![0u8; 65536];
            while !st.load(Ordering::Relaxed) {
                if let Ok((n, _)) = r.recv_from(&mut buf) {
                    back_message(&sh, &String::from_utf8_lossy(&buf[..n]));
                }
            }
        });
    }

    // Video.
    {
        let (sh, st) = (shared.clone(), stop.clone());
        let save = o.save.clone().or_else(|| (o.check || o.snapshot.is_some()).then(|| std::env::temp_dir().join("vhmd_link_sim.stream").to_string_lossy().into_owned()));
        let quiet = o.quiet;
        let save2 = save.clone();
        thread::spawn(move || video_loop(pc, sh, st, save2, quiet));
        // remember for the check
        *SAVE_PATH.lock().unwrap() = save;
    }

    // The display: one refresh per period; poses go out every refresh.
    let period = Duration::from_secs_f64(1.0 / o.rate);
    let period_ms = 1000.0 / o.rate;
    let mut next = start + period;
    let mut last_info = Instant::now() - Duration::from_secs(10);
    let mut last_stats = Instant::now();
    let mut last_timing = Instant::now();
    let mut last_print = Instant::now();
    let (mut shown_w, mut repeats_w, mut dropped_w) = (0u64, 0u64, 0u64);
    let mut waits: Vec<f64> = Vec::new();
    let (mut pc_sum, mut ps_sum, mut ph_n) = (0f64, 0f64, 0usize);
    let mut have_frame = false;
    let mut predict_ms = 0f64;
    let mut late_ms = 0f64;
    let mut prev_counts = [0u64; 6];
    let mut display_frames = 0u64;
    let mut link_stats = String::new();
    let mut lines = 0;
    let end = start + Duration::from_secs_f64(o.seconds);
    let pc_pose: SocketAddr = (pc, POSE_PORT).into();

    while Instant::now() < end {
        let now = Instant::now();
        if next > now {
            thread::sleep(next - now);
        }
        let vsync = next;
        next += period;
        if Instant::now() > next + period * 4 {
            next = Instant::now() + period; // fell behind: do not burst
        }
        display_frames += 1;
        let t_vsync = (vsync - start).as_secs_f64();

        // Show: the newest frame that arrived before this refresh.
        let mut shown_pose = None;
        {
            let mut s = shared.lock().unwrap();
            let mut newest: Option<Arrived> = None;
            while let Some(a) = s.queue.front() {
                if a.arrived > vsync {
                    break;
                }
                let a = s.queue.pop_front().unwrap();
                if newest.is_some() {
                    dropped_w += 1;
                    s.totals.dropped += 1;
                }
                newest = Some(a);
            }
            match newest {
                Some(a) => {
                    shown_w += 1;
                    s.totals.shown += 1;
                    let w = (vsync - a.arrived).as_secs_f64() * 1000.0;
                    waits.push(w);
                    s.totals.wait_sum += w;
                    let ph = 2.0 * PI * (w % period_ms) / period_ms;
                    pc_sum += ph.cos();
                    ps_sum += ph.sin();
                    ph_n += 1;
                    have_frame = true;
                    shown_pose = a.pose;
                    let _ = (a.index, a.arrived_us);
                }
                None => {
                    if have_frame {
                        repeats_w += 1;
                        s.totals.repeats += 1;
                    }
                }
            }
            // Look-ahead: which sent pose was this frame rendered with?
            if let Some(rp) = shown_pose {
                if let Some((late, ahead)) = match_sent(&s.sent, &rp, t_vsync) {
                    late_ms = late;
                    predict_ms += 0.05 * ((ahead + late).clamp(0.0, 80.0) - predict_ms);
                }
            }
        }

        // Poses for the PC: predicted for this headset's next refresh (+ look-ahead).
        let predict = shared.lock().unwrap().predict;
        let ahead = if predict { predict_ms } else { 0.0 };
        let pred_s = t_vsync + 2.0 / o.rate + ahead / 1000.0;
        let head = head_at(&o.motion, pred_s);
        let mut lh = head;
        lh.p = [head.p[0] - 0.2, head.p[1] - 0.35, head.p[2] - 0.3];
        let mut rh = head;
        rh.p = [head.p[0] + 0.2, head.p[1] - 0.35, head.p[2] - 0.3];
        let mut msg = String::with_capacity(1024);
        msg.push('[');
        msg.push_str(&pose_json("hmd", &head));
        msg.push(',');
        msg.push_str(&pose_json("left_hand", &lh));
        msg.push(',');
        msg.push_str(&pose_json("right_hand", &rh));
        if o.info && last_info.elapsed() >= Duration::from_secs(2) {
            last_info = Instant::now();
            // A Pico 4 Ultra's display (same numbers, so VHMD's settings stay as they are).
            msg.push_str(&format!(
                ",{{\"type\":\"headset\",\"model\":\"VHMD Link Simulator\",\"refresh\":{:.1},\"render\":[1920,1920],\
                 \"fov\":[[-0.82903,0.82903,0.82903,-0.82903],[-0.82903,0.82903,0.82903,-0.82903]],\
                 \"eyes\":[[-0.036,0,0,1,0,0,0],[0.036,0,0,1,0,0,0]],\"edition\":{},\"version\":\"0.6.0\"}}",
                o.rate,
                json_quote(&o.edition)
            ));
            if !link_stats.is_empty() {
                msg.push(',');
                msg.push_str(&link_stats);
            }
        }
        if last_timing.elapsed() >= Duration::from_millis(500) {
            last_timing = Instant::now();
            if ph_n >= 10 {
                let mut a = ps_sum.atan2(pc_sum);
                if a < 0.0 {
                    a += 2.0 * PI;
                }
                let slack = a / (2.0 * PI) * period_ms;
                let weight = (pc_sum * pc_sum + ps_sum * ps_sum).sqrt() / ph_n as f64;
                msg.push_str(&format!(",{{\"type\":\"link_timing\",\"slack_ms\":{slack:.2},\"period_ms\":{period_ms:.3},\"weight\":{weight:.3},\"n\":{ph_n}}}"));
            }
            pc_sum = 0.0;
            ps_sum = 0.0;
            ph_n = 0;
        }
        msg.push(']');
        let sent_us = now_us();
        if let Some(t) = tcp.as_mut() {
            msg.push('\n');
            if t.write_all(msg.as_bytes()).is_err() {
                eprintln!("PC closed the pose connection");
                break;
            }
        } else {
            let _ = udp.send_to(msg.as_bytes(), pc_pose);
        }
        {
            let mut s = shared.lock().unwrap();
            s.sent.push_back(Sent { sent_us, pred_s, ahead_ms: ahead, pose: head });
            while s.sent.len() > 400 {
                s.sent.pop_front();
            }
        }

        // Statistics, every 2 s for the PC (as the app's link_stats) and every second here.
        if last_stats.elapsed() >= Duration::from_secs(2) {
            let secs = last_stats.elapsed().as_secs_f64();
            last_stats = Instant::now();
            let (wa, wp) = avg_p95(&waits);
            let s = shared.lock().unwrap();
            // Counts over these 2 s, from the running totals.
            let t = &s.totals;
            let now_counts = [display_frames, t.frames, t.shown, t.repeats, t.dropped, t.audio_packets];
            let d: Vec<f64> = now_counts.iter().zip(prev_counts.iter()).map(|(a, b)| (a - b) as f64 / secs).collect();
            prev_counts = now_counts;
            link_stats = format!(
                "{{\"type\":\"link_stats\",\"display\":{:.1},\"received\":{:.1},\"decoded\":{:.1},\"shown\":{:.1},\"repeats\":{:.1},\
                 \"dropped\":{:.1},\"buffer\":0,\"wait_ms\":{wa:.1},\"wait_p95_ms\":{wp:.1},\"reprojected\":100,\"audio_packets\":{:.1},\
                 \"audio_underruns\":0,\"decode_ms\":0,\"decode_max_ms\":0,\"predict_ms\":{:.1},\"late_ms\":{late_ms:.1}}}",
                d[0], d[1], d[1], d[2], d[3], d[4], d[5],
                if s.predict { predict_ms } else { 0.0 }
            );
        }
        if last_print.elapsed() >= Duration::from_secs(1) {
            let secs = last_print.elapsed().as_secs_f64();
            last_print = Instant::now();
            let (wa, wp) = avg_p95(&waits);
            let mut s = shared.lock().unwrap();
            let (ta, _tp, tm, _) = s.transit.take();
            let (la, _lp, lm, ln) = s.pose_lat.take();
            if !o.quiet {
                if lines % 20 == 0 {
                    println!("  time | video fps  Mbit/s | transit ms avg/max | pose->frame ms avg/max | shown rep drop | wait ms avg/p95 | audio/s | ahead ms");
                }
                lines += 1;
                println!(
                    "{:6.1} | {:9.0} {:7.1} | {:7.2} / {:5.2}    | {:8.1} / {:5.1}{}     | {:5.0} {:3} {:4} | {:5.1} / {:4.1}   | {:7.0} | {:5.1}{}",
                    start.elapsed().as_secs_f64(),
                    s.w_frames as f64 / secs,
                    s.w_bytes as f64 * 8.0 / 1e6 / secs,
                    ta,
                    tm,
                    la,
                    lm,
                    if ln == 0 { " (still)" } else { "" },
                    shown_w as f64 / secs,
                    repeats_w,
                    dropped_w,
                    wa,
                    wp,
                    s.w_audio as f64 / secs,
                    if s.predict { predict_ms } else { 0.0 },
                    if s.connected { "" } else { "  (no video)" }
                );
            }
            s.w_frames = 0;
            s.w_bytes = 0;
            s.w_audio = 0;
            shown_w = 0;
            repeats_w = 0;
            dropped_w = 0;
            waits.clear();
        }
    }
    stop.store(true, Ordering::Relaxed);
    thread::sleep(Duration::from_millis(300));

    // Summary.
    let s = shared.lock().unwrap();
    let t = &s.totals;
    let secs = start.elapsed().as_secs_f64();
    let mut decode = String::from("not checked");
    if o.check || o.snapshot.is_some() {
        if let Some(path) = SAVE_PATH.lock().unwrap().clone() {
            decode = check_stream(&path, &t.codec, o.snapshot.as_deref());
        }
    }
    let summary = format!(
        "{{\"seconds\":{secs:.1},\"display_frames\":{display_frames},\"codec\":{},\"width\":{},\"height\":{},\"frames\":{},\"fps\":{:.2},\
         \"mbit_s\":{:.2},\"lost\":{},\"out_of_order\":{},\"transit_ms\":{:.3},\"transit_max_ms\":{:.3},\"pose_to_frame_ms\":{:.2},\
         \"pose_to_frame_max_ms\":{:.2},\"shown\":{},\"repeats\":{},\"dropped\":{},\"shown_per_s\":{:.2},\"wait_ms\":{:.2},\
         \"audio_packets\":{},\"audio_format\":{},\"haptics\":{},\"look_ahead_ms\":{:.1},\"late_ms\":{late_ms:.1},\"decode\":{},\"pc_status\":{},\"started_us\":{start_us}}}",
        json_quote(&t.codec),
        t.width,
        t.height,
        t.frames,
        t.frames as f64 / secs,
        t.bytes as f64 * 8.0 / 1e6 / secs,
        t.lost,
        t.out_of_order,
        if t.frames > 0 { t.transit_sum / t.frames as f64 } else { 0.0 },
        t.transit_max,
        if t.pose_lat_n > 0 { t.pose_lat_sum / t.pose_lat_n as f64 } else { 0.0 },
        t.pose_lat_max,
        t.shown,
        t.repeats,
        t.dropped,
        t.shown as f64 / secs,
        if t.shown > 0 { t.wait_sum / t.shown as f64 } else { 0.0 },
        t.audio_packets,
        json_quote(&t.audio_format),
        t.haptics,
        if s.predict { predict_ms } else { 0.0 },
        json_quote(&decode),
        json_quote(&t.status),
    );
    println!("summary: {summary}");
    if let Some(p) = &o.json {
        let _ = std::fs::write(p, &summary);
    }
    let ok = t.frames > 0;
    std::process::exit(if ok { 0 } else { 1 });
}

static SAVE_PATH: Mutex<Option<String>> = Mutex::new(None);

fn avg_p95(v: &[f64]) -> (f64, f64) {
    if v.is_empty() {
        return (0.0, 0.0);
    }
    let mut s = v.to_vec();
    s.sort_by(|a, b| a.partial_cmp(b).unwrap());
    (s.iter().sum::<f64>() / s.len() as f64, s[(s.len() * 95 / 100).min(s.len() - 1)])
}

/// The sent pose a frame was rendered with (nearest; only while the head turns,
/// so poses tell frames apart): (shown this much after its predicted time,
/// the look-ahead it had), both ms.
fn match_sent(sent: &VecDeque<Sent>, rendered: &Pose, display_s: f64) -> Option<(f64, f64)> {
    let mut best: Option<(usize, f64)> = None;
    for (i, s) in sent.iter().enumerate() {
        let d = ((s.pose.p[0] - rendered.p[0]).powi(2) + (s.pose.p[1] - rendered.p[1]).powi(2) + (s.pose.p[2] - rendered.p[2]).powi(2)).sqrt()
            + 0.05 * angle_between(&s.pose.q, &rendered.q);
        if best.map_or(true, |(_, bd)| d < bd) {
            best = Some((i, d));
        }
    }
    let (i, d) = best?;
    if i == 0 || d > 0.002 || angle_between(&sent[i].pose.q, &sent[i - 1].pose.q) < 0.0007 {
        return None;
    }
    let late = (display_s - sent[i].pred_s) * 1000.0;
    (-50.0..150.0).contains(&late).then_some((late, sent[i].ahead_ms))
}

/// What the PC sends back on the pose link: status, haptics.
fn back_message(sh: &Arc<Mutex<Shared>>, text: &str) {
    let mut s = sh.lock().unwrap();
    if text.contains("\"haptic\"") {
        s.totals.haptics += 1;
    } else if text.contains("\"type\":\"status\"") {
        s.predict = json_bool(text, "predict").unwrap_or(false);
        s.totals.status = text.trim().to_string();
    }
}

fn video_loop(pc: IpAddr, sh: Arc<Mutex<Shared>>, stop: Arc<AtomicBool>, save: Option<String>, quiet: bool) {
    let mut out = save.as_ref().and_then(|p| File::create(p).ok());
    let mut last_index = 0u64;
    while !stop.load(Ordering::Relaxed) {
        let mut conn = match TcpStream::connect_timeout(&(pc, VIDEO_PORT).into(), Duration::from_millis(500)) {
            Ok(c) => c,
            Err(_) => {
                thread::sleep(Duration::from_millis(300));
                continue;
            }
        };
        conn.set_nodelay(true).ok();
        conn.set_read_timeout(Some(Duration::from_millis(500))).ok();
        let mut hdr = [0u8; HEADER_SIZE];
        let mut payload = Vec::new();
        loop {
            if stop.load(Ordering::Relaxed) {
                return;
            }
            match read_full(&mut conn, &mut hdr, &stop) {
                Ok(true) => {}
                Ok(false) => break,
                Err(_) => break,
            }
            let Some(h) = Header::parse(&hdr) else { break };
            payload.resize(h.size as usize, 0);
            if h.size > 0 && !matches!(read_full(&mut conn, &mut payload, &stop), Ok(true)) {
                break;
            }
            let arrived = Instant::now();
            let arrived_us = now_us();
            let mut s = sh.lock().unwrap();
            s.connected = true;
            match h.kind {
                KIND_CONFIG => {
                    let text = String::from_utf8_lossy(&payload).into_owned();
                    s.totals.configs += 1;
                    s.totals.codec = json_string(&text, "codec").unwrap_or_default();
                    s.totals.width = json_number(&text, "width").unwrap_or(0.0) as u64;
                    s.totals.height = json_number(&text, "height").unwrap_or(0.0) as u64;
                    if !quiet {
                        println!("video: {} {}x{}", s.totals.codec, s.totals.width, s.totals.height);
                    }
                }
                KIND_FRAME => {
                    s.totals.frames += 1;
                    s.totals.bytes += h.size as u64;
                    s.w_frames += 1;
                    s.w_bytes += h.size as u64;
                    if last_index != 0 && h.index != 0 {
                        if h.index > last_index + 1 {
                            s.totals.lost += h.index - last_index - 1;
                        } else if h.index <= last_index {
                            s.totals.out_of_order += 1;
                        }
                    }
                    if h.index != 0 {
                        last_index = h.index;
                    }
                    let transit = (arrived_us as f64 - h.time_us as f64) / 1000.0;
                    s.transit.add(transit);
                    s.totals.transit_sum += transit;
                    s.totals.transit_max = s.totals.transit_max.max(transit);
                    let pose = h.pose_valid().then(|| Pose {
                        p: [h.pose[0] as f64, h.pose[1] as f64, h.pose[2] as f64],
                        q: [h.pose[3] as f64, h.pose[4] as f64, h.pose[5] as f64, h.pose[6] as f64],
                    });
                    // Pose -> frame: when was the pose this frame was rendered with sent?
                    if let Some(rp) = pose {
                        let mut best: Option<(u64, f64)> = None;
                        let mut moving = false;
                        let n = s.sent.len();
                        for (i, x) in s.sent.iter().enumerate() {
                            let d = ((x.pose.p[0] - rp.p[0]).powi(2) + (x.pose.p[1] - rp.p[1]).powi(2) + (x.pose.p[2] - rp.p[2]).powi(2)).sqrt()
                                + 0.05 * angle_between(&x.pose.q, &rp.q);
                            if best.map_or(true, |(_, bd)| d < bd) {
                                best = Some((x.sent_us, d));
                                moving = i > 0 && i < n && angle_between(&x.pose.q, &s.sent[i - 1].pose.q) > 0.0007;
                            }
                        }
                        if let Some((sent_us, d)) = best {
                            if moving && d < 0.002 {
                                let lat = (arrived_us as f64 - sent_us as f64) / 1000.0;
                                s.pose_lat.add(lat);
                                s.totals.pose_lat_sum += lat;
                                s.totals.pose_lat_n += 1;
                                s.totals.pose_lat_max = s.totals.pose_lat_max.max(lat);
                            }
                        }
                    }
                    s.queue.push_back(Arrived { index: h.index, arrived, arrived_us, pose });
                    while s.queue.len() > 8 {
                        s.queue.pop_front();
                    }
                    if let Some(f) = out.as_mut() {
                        let _ = f.write_all(&payload);
                    }
                }
                KIND_AUDIO => {
                    s.totals.audio_packets += 1;
                    s.w_audio += 1;
                }
                KIND_AUDIO_FORMAT => s.totals.audio_format = String::from_utf8_lossy(&payload).into_owned(),
                _ => {}
            }
        }
        sh.lock().unwrap().connected = false;
        if !quiet {
            println!("video: connection ended; reconnecting");
        }
    }
}

/// Fills buf; Ok(false) when the connection ended or the run stopped.
fn read_full(c: &mut TcpStream, buf: &mut [u8], stop: &AtomicBool) -> std::io::Result<bool> {
    let mut got = 0;
    while got < buf.len() {
        match c.read(&mut buf[got..]) {
            Ok(0) => return Ok(false),
            Ok(n) => got += n,
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock || e.kind() == std::io::ErrorKind::TimedOut => {
                if stop.load(Ordering::Relaxed) {
                    return Ok(false);
                }
            }
            Err(e) => return Err(e),
        }
    }
    Ok(true)
}

/// Decodes the saved stream with ffmpeg: frames decoded and errors.
fn check_stream(path: &str, codec: &str, snapshot: Option<&str>) -> String {
    let fmt = match codec {
        "hevc" => "hevc",
        "av1" => "obu",
        _ => "h264",
    };
    let r = Command::new("ffmpeg")
        .args(["-hide_banner", "-loglevel", "error", "-f", fmt, "-i", path, "-f", "null", "-"])
        .output();
    let mut text = match r {
        Ok(o) => {
            let err = String::from_utf8_lossy(&o.stderr);
            let errors = err.lines().filter(|l| !l.trim().is_empty()).count();
            format!("ffmpeg: {} error line(s){}", errors, err.lines().next().map(|l| format!(" (first: {l})")).unwrap_or_default())
        }
        Err(e) => format!("ffmpeg not available: {e}"),
    };
    if let Some(png) = snapshot {
        let r = Command::new("ffmpeg")
            .args(["-hide_banner", "-loglevel", "error", "-y", "-f", fmt, "-i", path, "-vf", "select=eq(n\\,30)", "-frames:v", "1", png])
            .status();
        text.push_str(&format!("; snapshot {}", if matches!(r, Ok(s) if s.success()) { png } else { "failed" }));
    }
    text
}
