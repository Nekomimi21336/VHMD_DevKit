//! Video over UDP with forward error correction (VHMD Link, optional).
//!
//! Over TCP a lost Wi-Fi packet holds everything behind it until it is sent
//! again (tens of milliseconds). Over UDP each message (the same 64-byte
//! "VHV1" header + payload as on TCP) is cut into fragments, and parity
//! fragments let the receiver rebuild lost ones without waiting.
//!
//! Packet (little endian):
//!
//! ```text
//!  0  "VHU2"
//!  4  u32 message sequence number (+1 per message)
//!  8  u16 fragment index: 0..data_count-1 data, data_count.. parity
//! 10  u16 data_count
//! 12  u8  fec           parity fragments per 100 data fragments (rounded up per block; 0 = none)
//! 13  u8  kind          the message's kind (2 = video frame, 3 = sound, ...): a receiver
//!                       that loses a frame needs a keyframe, a lost sound packet does not
//! 14  u16 frag_size     bytes per data fragment (the last one may be shorter)
//! 16  u32 message size
//! 20  fragment bytes
//! ```
//!
//! Parity: Reed-Solomon over GF(2^8) (polynomial 0x11D) with a Cauchy matrix.
//! The data fragments are taken in blocks of BLOCK (the last may be smaller);
//! a block of k data fragments has m = ceil(k * fec / 100) parity fragments,
//! numbered after all data fragments, block by block. Any k of a block's k + m
//! fragments rebuild it. Parity row i, data column j: 1 / (x_i + y_j) with
//! x_i = i, y_j = m + j (all fragments zero-padded to frag_size).

use std::collections::{BTreeMap, VecDeque};
use std::time::{Duration, Instant};

pub const UDP_MAGIC: &[u8; 4] = b"VHU2";
pub const UDP_HEADER: usize = 20;
pub const DEFAULT_FRAG: usize = 1200;
pub const BLOCK: usize = 128;

// -- GF(2^8) ----------------------------------------------------------------------
struct Gf {
    exp: [u8; 512],
    log: [u8; 256],
}

fn gf() -> &'static Gf {
    use std::sync::OnceLock;
    static G: OnceLock<Gf> = OnceLock::new();
    G.get_or_init(|| {
        let mut g = Gf { exp: [0; 512], log: [0; 256] };
        let mut x: u16 = 1;
        for i in 0..255 {
            g.exp[i] = x as u8;
            g.log[x as usize] = i as u8;
            x <<= 1;
            if x & 0x100 != 0 {
                x ^= 0x11D;
            }
        }
        for i in 255..512 {
            g.exp[i] = g.exp[i - 255];
        }
        g
    })
}

fn mul(a: u8, b: u8) -> u8 {
    if a == 0 || b == 0 {
        return 0;
    }
    let g = gf();
    g.exp[g.log[a as usize] as usize + g.log[b as usize] as usize]
}

fn inv(a: u8) -> u8 {
    let g = gf();
    g.exp[255 - g.log[a as usize] as usize]
}

/// dst ^= c * src (byte-wise), with a 256-entry table for c.
fn mul_add(dst: &mut [u8], src: &[u8], c: u8) {
    if c == 0 {
        return;
    }
    let mut t = [0u8; 256];
    for (v, e) in t.iter_mut().enumerate() {
        *e = mul(c, v as u8);
    }
    for (d, s) in dst.iter_mut().zip(src) {
        *d ^= t[*s as usize];
    }
}

fn cauchy(i: usize, j: usize, m: usize) -> u8 {
    inv((i as u8) ^ ((m + j) as u8))
}

/// (first data index, data count, parity count) of each block.
pub fn blocks(data_count: usize, fec: usize) -> Vec<(usize, usize, usize)> {
    let mut out = Vec::new();
    let mut at = 0;
    while at < data_count {
        let k = BLOCK.min(data_count - at);
        let m = if fec == 0 { 0 } else { (k * fec).div_ceil(100).clamp(1, 255 - k) };
        out.push((at, k, m));
        at += k;
    }
    out
}

/// Inverts an n x n matrix over GF(2^8) (rows), or None if singular.
fn invert(mut a: Vec<Vec<u8>>) -> Option<Vec<Vec<u8>>> {
    let n = a.len();
    let mut b: Vec<Vec<u8>> = (0..n).map(|i| (0..n).map(|j| (i == j) as u8).collect()).collect();
    for c in 0..n {
        let p = (c..n).find(|&r| a[r][c] != 0)?;
        a.swap(c, p);
        b.swap(c, p);
        let f = inv(a[c][c]);
        for x in 0..n {
            a[c][x] = mul(a[c][x], f);
            b[c][x] = mul(b[c][x], f);
        }
        for r in 0..n {
            if r != c && a[r][c] != 0 {
                let f = a[r][c];
                let (ac, bc) = (a[c].clone(), b[c].clone());
                for x in 0..n {
                    a[r][x] ^= mul(f, ac[x]);
                    b[r][x] ^= mul(f, bc[x]);
                }
            }
        }
    }
    Some(b)
}

// -- sending ------------------------------------------------------------------------
/// Cuts messages into packets.
pub struct Packetizer {
    seq: u32,
    pub frag_size: usize,
    pub fec: u32,
}

impl Packetizer {
    pub fn new(frag_size: usize, fec: u32) -> Packetizer {
        Packetizer { seq: 0, frag_size: frag_size.clamp(64, 1400), fec }
    }

    /// The packets of one message; `fec` overrides the default for it (e.g. more for keyframes).
    pub fn packets_with(&mut self, msg: &[u8], fec: u32, kind: u8) -> Vec<Vec<u8>> {
        let fec = fec.min(200);
        self.seq = self.seq.wrapping_add(1);
        let fs = self.frag_size;
        let data_count = msg.len().div_ceil(fs).max(1);
        let head = |index: usize| {
            let mut p = Vec::with_capacity(UDP_HEADER + fs);
            p.extend_from_slice(UDP_MAGIC);
            p.extend_from_slice(&self.seq.to_le_bytes());
            p.extend_from_slice(&(index as u16).to_le_bytes());
            p.extend_from_slice(&(data_count as u16).to_le_bytes());
            p.push(fec as u8);
            p.push(kind);
            p.extend_from_slice(&(fs as u16).to_le_bytes());
            p.extend_from_slice(&(msg.len() as u32).to_le_bytes());
            p
        };
        let frag = |i: usize| &msg[i * fs..((i + 1) * fs).min(msg.len())];
        let mut out = Vec::new();
        for i in 0..data_count {
            let mut p = head(i);
            p.extend_from_slice(frag(i));
            out.push(p);
        }
        let mut next = data_count;
        for (start, k, m) in blocks(data_count, fec as usize) {
            for i in 0..m {
                let mut par = vec![0u8; fs];
                for j in 0..k {
                    mul_add(&mut par, frag(start + j), cauchy(i, j, m));
                }
                let mut p = head(next);
                p.extend_from_slice(&par);
                out.push(p);
                next += 1;
            }
        }
        out
    }

    pub fn packets(&mut self, msg: &[u8]) -> Vec<Vec<u8>> {
        self.packets_with(msg, self.fec, 2)
    }
}

// -- receiving ----------------------------------------------------------------------
struct Partial {
    data_count: usize,
    fec: usize,
    kind: u8,
    frag_size: usize,
    size: usize,
    frags: Vec<Option<Vec<u8>>>, // data then parity
    created: Instant,
}

impl Partial {
    /// The message, if every block can be completed (bool: parity was used).
    fn complete(&mut self) -> Option<(Vec<u8>, bool)> {
        let mut recovered = false;
        let mut parity_at = self.data_count;
        for (start, k, m) in blocks(self.data_count, self.fec) {
            let missing: Vec<usize> = (0..k).filter(|&j| self.frags[start + j].is_none()).collect();
            if !missing.is_empty() {
                let have_parity: Vec<usize> = (0..m).filter(|&i| self.frags[parity_at + i].is_some()).collect();
                if have_parity.len() < missing.len() {
                    return None;
                }
                // Rows: the data fragments we have (identity) and enough parity rows.
                let used: Vec<usize> = have_parity[..missing.len()].to_vec();
                let mut rows: Vec<Vec<u8>> = Vec::with_capacity(k);
                let mut srcs: Vec<Vec<u8>> = Vec::with_capacity(k);
                let mut pi = 0;
                for j in 0..k {
                    if let Some(f) = &self.frags[start + j] {
                        rows.push((0..k).map(|c| (c == j) as u8).collect());
                        let mut v = f.clone();
                        v.resize(self.frag_size, 0);
                        srcs.push(v);
                    } else {
                        let i = used[pi];
                        pi += 1;
                        rows.push((0..k).map(|c| cauchy(i, c, m)).collect());
                        let mut v = self.frags[parity_at + i].clone().unwrap();
                        v.resize(self.frag_size, 0);
                        srcs.push(v);
                    }
                }
                let inv_m = invert(rows)?;
                for &j in &missing {
                    let mut out = vec![0u8; self.frag_size];
                    for (c, s) in srcs.iter().enumerate() {
                        mul_add(&mut out, s, inv_m[j][c]);
                    }
                    let idx = start + j;
                    let len = if idx + 1 == self.data_count { self.size - idx * self.frag_size } else { self.frag_size };
                    out.truncate(len);
                    self.frags[idx] = Some(out);
                }
                recovered = true;
            }
            parity_at += m;
        }
        let mut msg = Vec::with_capacity(self.size);
        for f in &self.frags[..self.data_count] {
            msg.extend_from_slice(f.as_ref()?);
        }
        msg.truncate(self.size);
        Some((msg, recovered))
    }

    fn parity_total(&self) -> usize {
        blocks(self.data_count, self.fec).iter().map(|b| b.2).sum()
    }
}

/// Puts messages back together. Messages come out in order; one that cannot
/// be completed by the time a newer one is (or within `wait`) counts as lost.
pub struct Reassembler {
    partial: BTreeMap<u32, Partial>,
    last_out: Option<u32>,
    pub wait: Duration,
    pub packets: u64,
    pub delivered: u64,
    pub recovered: u64, // messages that needed parity
    pub lost: u64,
    lost_seqs: VecDeque<(u32, u8)>,
}

impl Default for Reassembler {
    fn default() -> Self {
        Reassembler {
            partial: BTreeMap::new(),
            last_out: None,
            wait: Duration::from_millis(100),
            packets: 0,
            delivered: 0,
            recovered: 0,
            lost: 0,
            lost_seqs: VecDeque::new(),
        }
    }
}

fn newer(a: u32, b: u32) -> bool {
    (a.wrapping_sub(b) as i32) > 0
}

impl Reassembler {
    /// One packet; the message it completes, if any.
    pub fn push(&mut self, pkt: &[u8], now: Instant) -> Option<Vec<u8>> {
        if pkt.len() < UDP_HEADER || &pkt[0..4] != UDP_MAGIC {
            return None;
        }
        self.packets += 1;
        let u16_at = |o: usize| u16::from_le_bytes([pkt[o], pkt[o + 1]]) as usize;
        let seq = u32::from_le_bytes(pkt[4..8].try_into().unwrap());
        let (index, data_count, fec, kind, frag_size) = (u16_at(8), u16_at(10), pkt[12] as usize, pkt[13], u16_at(14));
        let size = u32::from_le_bytes(pkt[16..20].try_into().unwrap()) as usize;
        if let Some(last) = self.last_out {
            if !newer(seq, last) {
                return None; // already delivered or given up
            }
        }
        if data_count == 0 || frag_size == 0 || size > data_count * frag_size {
            return None;
        }
        let p = self.partial.entry(seq).or_insert_with(|| Partial {
            data_count,
            fec,
            kind,
            frag_size,
            size,
            frags: Vec::new(),
            created: now,
        });
        if p.frags.is_empty() {
            let total = data_count + p.parity_total();
            p.frags = vec![None; total];
        }
        if index >= p.frags.len() || p.data_count != data_count {
            return None;
        }
        if p.frags[index].is_none() {
            p.frags[index] = Some(pkt[UDP_HEADER..].to_vec());
        }
        // Quick check: enough fragments for every block before trying.
        let (msg, rec) = p.complete()?;
        self.partial.remove(&seq);
        let older: Vec<u32> = self.partial.keys().copied().filter(|&s| newer(seq, s)).collect();
        for s in older {
            if let Some(p) = self.partial.remove(&s) {
                self.note_lost(s, p.kind);
            }
        }
        self.last_out = Some(seq);
        self.delivered += 1;
        self.recovered += rec as u64;
        Some(msg)
    }

    /// Gives up on messages older than `wait`; returns how many were lost.
    pub fn expire(&mut self, now: Instant) -> u64 {
        let old: Vec<u32> = self.partial.iter().filter(|(_, p)| now - p.created > self.wait).map(|(s, _)| *s).collect();
        for s in &old {
            if let Some(p) = self.partial.remove(s) {
                self.note_lost(*s, p.kind);
            }
        }
        old.len() as u64
    }

    fn note_lost(&mut self, s: u32, kind: u8) {
        self.lost += 1;
        self.lost_seqs.push_back((s, kind));
        if self.lost_seqs.len() > 64 {
            self.lost_seqs.pop_front();
        }
    }

    /// Lost since the last call, (sequence, kind) (a frame lost means: wait for a keyframe).
    pub fn take_lost(&mut self) -> Vec<(u32, u8)> {
        self.lost_seqs.drain(..).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn msg(n: usize, seed: u8) -> Vec<u8> {
        (0..n).map(|i| (i as u8).wrapping_mul(31).wrapping_add(seed)).collect()
    }

    fn deliver(pk: &[Vec<u8>], drop: &dyn Fn(usize) -> bool, r: &mut Reassembler) -> Option<Vec<u8>> {
        let mut out = None;
        for (i, k) in pk.iter().enumerate() {
            if drop(i) {
                continue;
            }
            if let Some(x) = r.push(k, Instant::now()) {
                out = Some(x);
            }
        }
        out
    }

    #[test]
    fn gf_basics() {
        for a in 1..=255u8 {
            assert_eq!(mul(a, inv(a)), 1);
        }
        assert_eq!(mul(0x53, 0xCA), mul(0xCA, 0x53));
    }

    #[test]
    fn no_loss() {
        let mut p = Packetizer::new(1200, 20);
        let m = msg(70_000, 1);
        let mut r = Reassembler::default();
        assert_eq!(deliver(&p.packets(&m), &|_| false, &mut r).as_deref(), Some(&m[..]));
        assert_eq!(r.recovered, 0);
    }

    #[test]
    fn any_m_losses_per_block() {
        let mut p = Packetizer::new(1000, 10);
        let m = msg(59_500, 7); // 60 data fragments, 6 parity
        let pk = p.packets(&m);
        assert_eq!(pk.len(), 66);
        // six scattered losses, the last (short) fragment among them
        let lost = [3usize, 4, 17, 40, 41, 59];
        let mut r = Reassembler::default();
        assert_eq!(deliver(&pk, &|i| lost.contains(&i), &mut r).as_deref(), Some(&m[..]));
        assert_eq!(r.recovered, 1);
        // seven is one too many
        let m2 = msg(59_500, 8);
        let pk = p.packets(&m2);
        let lost = [0usize, 1, 2, 3, 4, 5, 6];
        let mut r = Reassembler::default();
        assert!(deliver(&pk, &|i| lost.contains(&i), &mut r).is_none());
    }

    #[test]
    fn several_blocks_and_parity_losses() {
        let mut p = Packetizer::new(1000, 25);
        let m = msg(300_000, 5); // 300 data: blocks 128 + 128 + 44
        let pk = p.packets(&m);
        let b = blocks(300, 25);
        assert_eq!(b, vec![(0, 128, 32), (128, 128, 32), (256, 44, 11)]);
        assert_eq!(pk.len(), 300 + 75);
        // 30 losses in the first block (data and parity), 11 in the last
        let lost: Vec<usize> = (0..20).chain(300..310).chain(256..267).collect();
        let mut r = Reassembler::default();
        assert_eq!(deliver(&pk, &|i| lost.contains(&i), &mut r).as_deref(), Some(&m[..]));
    }

    #[test]
    fn lost_message_and_order() {
        let mut p = Packetizer::new(1000, 10);
        let a = msg(20_000, 1);
        let b = msg(20_000, 2);
        let pa = p.packets(&a);
        let pb = p.packets(&b);
        let mut r = Reassembler::default();
        assert!(deliver(&pa, &|i| i < 3, &mut r).is_none()); // 3 lost, 2 parity: lost
        assert_eq!(deliver(&pb, &|_| false, &mut r).as_deref(), Some(&b[..]));
        assert_eq!(r.lost, 1);
        assert!(r.push(&pa[0], Instant::now()).is_none());
        assert_eq!(r.take_lost().len(), 1);
    }

    /// Packets for the C++ receiver's test (tests/unit/test_udp_video.cpp):
    /// written to link/proto/testdata/udp_vectors.bin when VHMD_WRITE_VECTORS is set.
    #[test]
    fn vectors() {
        let mut p = Packetizer::new(1000, 25);
        let m = msg(300_000, 3);
        let pk = p.packets(&m);
        let lost: Vec<usize> = (0..20).chain(300..310).chain(256..267).collect();
        if std::env::var("VHMD_WRITE_VECTORS").is_ok() {
            let mut f = Vec::new();
            f.extend_from_slice(&(m.len() as u32).to_le_bytes());
            f.extend_from_slice(&m);
            let kept: Vec<&Vec<u8>> = pk.iter().enumerate().filter(|(i, _)| !lost.contains(i)).map(|(_, k)| k).collect();
            f.extend_from_slice(&(kept.len() as u32).to_le_bytes());
            for k in kept {
                f.extend_from_slice(&(k.len() as u32).to_le_bytes());
                f.extend_from_slice(k);
            }
            std::fs::create_dir_all(concat!(env!("CARGO_MANIFEST_DIR"), "/testdata")).unwrap();
            std::fs::write(concat!(env!("CARGO_MANIFEST_DIR"), "/testdata/udp_vectors.bin"), f).unwrap();
        }
        assert_eq!(pk.len(), 375);
    }

    #[test]
    fn encode_speed() {
        // A 40 Mbit/s frame at 72 fps (~70 KB) with 20 % parity: well under a millisecond.
        let mut p = Packetizer::new(1200, 20);
        let m = msg(70_000, 9);
        let t = Instant::now();
        for _ in 0..20 {
            std::hint::black_box(p.packets(&m));
        }
        let per = t.elapsed() / 20;
        println!("encode: {:?} per 70 KB frame", per);
        assert!(per < Duration::from_millis(20));
    }
}
