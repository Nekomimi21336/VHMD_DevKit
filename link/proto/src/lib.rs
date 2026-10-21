//! VHMD Link protocol shared by the relay and the simulator.
//!
//! - Ports: poses to the PC on UDP / TCP 39580, video and sound from the PC on
//!   TCP 39581, discovery on UDP 39582 ("VHMD-DISCOVER"), the driver's encoder
//!   on 127.0.0.1:39583, the driver's message port on 127.0.0.1:39570.
//! - Video framing: a 64-byte little-endian header ("VHV1", kind, size, flags,
//!   frame index, time in microseconds, render pose x y z qw qx qy qz) and the
//!   payload (see driver/src/windows/stream_encoder.h).
//! - Pose messages: one JSON array of VHMD protocol messages per UDP datagram
//!   (or per line over TCP).

use std::time::{SystemTime, UNIX_EPOCH};

pub mod udp;

/// Sets a socket's send (SO_SNDBUF) or receive (SO_RCVBUF) buffer. A video
/// frame arrives as a burst of packets (70 KB and more): with the default
/// 64 KB the system drops some before they are read.
pub fn set_socket_buffer<S: SocketHandle>(s: &S, send: bool, bytes: i32) {
    s.set_buffer(send, bytes);
}

pub trait SocketHandle {
    fn set_buffer(&self, send: bool, bytes: i32);
}

#[cfg(windows)]
mod sockbuf {
    use std::os::windows::io::AsRawSocket;
    #[link(name = "ws2_32")]
    extern "system" {
        fn setsockopt(s: usize, level: i32, name: i32, val: *const i32, len: i32) -> i32;
    }
    pub fn set(raw: usize, send: bool, bytes: i32) {
        unsafe {
            setsockopt(raw, 0xffff, if send { 0x1001 } else { 0x1002 }, &bytes, 4);
        }
    }
    impl super::SocketHandle for std::net::UdpSocket {
        fn set_buffer(&self, send: bool, bytes: i32) {
            set(self.as_raw_socket() as usize, send, bytes)
        }
    }
    impl super::SocketHandle for std::net::TcpStream {
        fn set_buffer(&self, send: bool, bytes: i32) {
            set(self.as_raw_socket() as usize, send, bytes)
        }
    }
}

#[cfg(unix)]
mod sockbuf {
    use std::os::unix::io::AsRawFd;
    extern "C" {
        fn setsockopt(s: i32, level: i32, name: i32, val: *const i32, len: u32) -> i32;
    }
    pub fn set(fd: i32, send: bool, bytes: i32) {
        unsafe {
            setsockopt(fd, 1, if send { 7 } else { 8 }, &bytes, 4); // SOL_SOCKET, SO_SNDBUF / SO_RCVBUF (Linux)
        }
    }
    impl super::SocketHandle for std::net::UdpSocket {
        fn set_buffer(&self, send: bool, bytes: i32) {
            set(self.as_raw_fd(), send, bytes)
        }
    }
    impl super::SocketHandle for std::net::TcpStream {
        fn set_buffer(&self, send: bool, bytes: i32) {
            set(self.as_raw_fd(), send, bytes)
        }
    }
}

pub const POSE_PORT: u16 = 39580;
pub const VIDEO_PORT: u16 = 39581;
pub const DISCOVERY_PORT: u16 = 39582;
pub const DRIVER_STREAM_PORT: u16 = 39583;
pub const DRIVER_PORT: u16 = 39570;

pub const KIND_CONFIG: u32 = 1;
pub const KIND_FRAME: u32 = 2;
pub const KIND_AUDIO: u32 = 3;
pub const KIND_AUDIO_FORMAT: u32 = 4;

pub const HEADER_SIZE: usize = 64;

/// The video message header.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Header {
    pub kind: u32,
    pub size: u32,
    pub flags: u32, // bit 0: pose valid
    pub index: u64,
    pub time_us: u64, // when the driver sent it (wall clock, now_us())
    pub pose: [f32; 7],
}

impl Header {
    pub fn parse(b: &[u8; HEADER_SIZE]) -> Option<Header> {
        if &b[0..4] != b"VHV1" {
            return None;
        }
        let u32_at = |o: usize| u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
        let u64_at = |o: usize| u64::from_le_bytes(b[o..o + 8].try_into().unwrap());
        let mut pose = [0f32; 7];
        for (i, p) in pose.iter_mut().enumerate() {
            *p = f32::from_le_bytes(b[32 + i * 4..36 + i * 4].try_into().unwrap());
        }
        Some(Header { kind: u32_at(4), size: u32_at(8), flags: u32_at(12), index: u64_at(16), time_us: u64_at(24), pose })
    }

    pub fn write(&self, b: &mut [u8; HEADER_SIZE]) {
        b[0..4].copy_from_slice(b"VHV1");
        b[4..8].copy_from_slice(&self.kind.to_le_bytes());
        b[8..12].copy_from_slice(&self.size.to_le_bytes());
        b[12..16].copy_from_slice(&self.flags.to_le_bytes());
        b[16..24].copy_from_slice(&self.index.to_le_bytes());
        b[24..32].copy_from_slice(&self.time_us.to_le_bytes());
        for (i, p) in self.pose.iter().enumerate() {
            b[32 + i * 4..36 + i * 4].copy_from_slice(&p.to_le_bytes());
        }
        b[60..64].copy_from_slice(&0u32.to_le_bytes());
    }

    pub fn pose_valid(&self) -> bool {
        self.flags & 1 != 0
    }
}

/// Wall clock in microseconds since 1970, the clock the driver stamps frames
/// with (GetSystemTimePreciseAsFileTime on Windows).
pub fn now_us() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_micros() as u64).unwrap_or(0)
}

// -- small JSON helpers -----------------------------------------------------------
// Messages here are small and flat; these read one value by key without a full
// parser (the same approach as the headset app).

fn value_start<'a>(s: &'a str, key: &str) -> Option<&'a str> {
    let k = format!("\"{}\"", key);
    let mut from = 0;
    while let Some(p) = s[from..].find(&k) {
        let after = &s[from + p + k.len()..];
        let t = after.trim_start();
        if let Some(rest) = t.strip_prefix(':') {
            return Some(rest.trim_start());
        }
        from += p + k.len();
    }
    None
}

pub fn json_number(s: &str, key: &str) -> Option<f64> {
    let v = value_start(s, key)?;
    let end = v.find(|c: char| !(c.is_ascii_digit() || "+-.eE".contains(c))).unwrap_or(v.len());
    v[..end].parse().ok()
}

pub fn json_string(s: &str, key: &str) -> Option<String> {
    let v = value_start(s, key)?;
    let v = v.strip_prefix('"')?;
    let mut out = String::new();
    let mut esc = false;
    for c in v.chars() {
        if esc {
            out.push(match c {
                'n' => '\n',
                't' => '\t',
                c => c,
            });
            esc = false;
        } else if c == '\\' {
            esc = true;
        } else if c == '"' {
            return Some(out);
        } else {
            out.push(c);
        }
    }
    None
}

pub fn json_bool(s: &str, key: &str) -> Option<bool> {
    let v = value_start(s, key)?;
    if v.starts_with("true") {
        Some(true)
    } else if v.starts_with("false") {
        Some(false)
    } else {
        None
    }
}

/// The numbers of an array value ("key": [1, 2.5, ...]); empty if absent.
pub fn json_numbers(s: &str, key: &str) -> Vec<f64> {
    let Some(v) = value_start(s, key) else { return Vec::new() };
    let Some(v) = v.strip_prefix('[') else { return Vec::new() };
    let end = v.find(']').unwrap_or(v.len());
    v[..end].split(',').filter_map(|x| x.trim().parse().ok()).collect()
}

/// A JSON string literal (with quotes).
pub fn json_quote(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// The top-level objects of a JSON array, as text slices.
pub fn array_objects(s: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let (mut depth, mut start, mut in_str, mut esc) = (0i32, 0usize, false, false);
    for (i, c) in s.char_indices() {
        if in_str {
            if esc {
                esc = false;
            } else if c == '\\' {
                esc = true;
            } else if c == '"' {
                in_str = false;
            }
            continue;
        }
        match c {
            '"' => in_str = true,
            '{' => {
                if depth == 0 {
                    start = i;
                }
                depth += 1;
            }
            '}' => {
                depth -= 1;
                if depth == 0 {
                    out.push(&s[start..=i]);
                }
            }
            _ => {}
        }
    }
    out
}

/// The "type" of a message object.
pub fn message_type(obj: &str) -> Option<String> {
    json_string(obj, "type")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_round_trip() {
        let h = Header { kind: KIND_FRAME, size: 1234, flags: 1, index: 77, time_us: 1_700_000_000_000_000, pose: [0.1, 1.6, -0.2, 1.0, 0.0, 0.0, 0.0] };
        let mut b = [0u8; HEADER_SIZE];
        h.write(&mut b);
        assert_eq!(Header::parse(&b), Some(h));
        b[0] = b'X';
        assert_eq!(Header::parse(&b), None);
    }

    #[test]
    fn json_values() {
        let s = r#"{"type":"link_timing","slack_ms":4.25,"period_ms":13.889,"name":"a \"b\"","ok":true,"n":-3e2}"#;
        assert_eq!(json_number(s, "slack_ms"), Some(4.25));
        assert_eq!(json_number(s, "n"), Some(-300.0));
        assert_eq!(json_string(s, "type").as_deref(), Some("link_timing"));
        assert_eq!(json_string(s, "name").as_deref(), Some("a \"b\""));
        assert_eq!(json_bool(s, "ok"), Some(true));
        assert_eq!(json_number(s, "missing"), None);
        assert_eq!(json_numbers(r#"{"foveation": [0.4, 4,0.5,2]}"#, "foveation"), vec![0.4, 4.0, 0.5, 2.0]);
        assert!(json_numbers(s, "missing").is_empty());
    }

    #[test]
    fn array_split() {
        let a = r#"[{"type":"pose","pos":[1,2,3]},{"type":"headset","model":"x}{"},{"t":{"n":1}}]"#;
        let o = array_objects(a);
        assert_eq!(o.len(), 3);
        assert_eq!(message_type(o[1]).as_deref(), Some("headset"));
        assert_eq!(o[2], r#"{"t":{"n":1}}"#);
        assert_eq!(json_quote("a\"b\n"), r#""a\"b\n""#);
    }
}
