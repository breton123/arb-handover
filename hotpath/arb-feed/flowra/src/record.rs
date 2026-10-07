//! Append-only FLOWRA1 capture. Join later by 64-byte signature.
//!
//! File:  ~/captures/flowra/flowra-<utc>.flw
//! Index: same stem .idx   (sig[64] | file_off[u64] | t_rx_mono_ns | wall_utc_ns)
//!
//! Record (little-endian):
//!   u32  type     1=first  2=dup
//!   u32  nbytes   remaining after this field (not including type+nbytes)
//!   u64  t_flowra_rx_mono_ns     CLOCK_MONOTONIC_RAW
//!   u64  t_flowra_rx_tsc         RDTSCP
//!   u64  wall_utc_ns
//!   i64  server_side_sec
//!   i32  server_side_nsec
//!   i64  expiration_sec
//!   i32  expiration_nsec
//!   u64  sender_stake
//!   u32  port
//!   u32  flags
//!   u32  tx_len
//!   u16  addr_len
//!   u16  reserved
//!   u8   sig[64]
//!   u8   tx[tx_len]
//!   u8   addr[addr_len]
//!
//! T_FLOWRA_RX is the mono/tsc pair. Tomorrow:
//!   lead_us = T_ORBITFLARE_FIRST_ACTIONABLE - T_FLOWRA_RX

use std::fs::{File, OpenOptions};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use std::sync::mpsc::Receiver;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

pub const MAGIC: &[u8; 8] = b"FLOWRA1\n";
pub const FILE_VERSION: u16 = 1;
pub const REC_FIRST: u32 = 1;
pub const REC_DUP: u32 = 2;
pub const HEADER_LEN: u64 = 128;

pub const FLAG_DISCARD: u32 = 1 << 0;
pub const FLAG_FORWARDED: u32 = 1 << 1;
pub const FLAG_REPAIR: u32 = 1 << 2;
pub const FLAG_VOTE: u32 = 1 << 3;
pub const FLAG_TRACER: u32 = 1 << 4;
pub const FLAG_STAKED: u32 = 1 << 5;

#[derive(Clone)]
pub struct Rec {
    pub first: bool,
    pub t_mono_ns: u64,
    pub t_tsc: u64,
    pub wall_utc_ns: u64,
    pub server_side_sec: i64,
    pub server_side_nsec: i32,
    pub expiration_sec: i64,
    pub expiration_nsec: i32,
    pub sender_stake: u64,
    pub port: u32,
    pub flags: u32,
    pub sig: [u8; 64],
    pub tx: Vec<u8>,
    pub addr: Vec<u8>,
}

pub struct Paths {
    pub dir: PathBuf,
    pub flw: PathBuf,
    pub idx: PathBuf,
    pub meta: PathBuf,
}

pub fn paths_for(dir: &Path) -> Paths {
    let wall = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let stem = format!("flowra-{wall}");
    Paths {
        dir: dir.to_path_buf(),
        flw: dir.join(format!("{stem}.flw")),
        idx: dir.join(format!("{stem}.idx")),
        meta: dir.join(format!("{stem}.meta.json")),
    }
}

fn write_u16(w: &mut impl Write, v: u16) -> std::io::Result<()> {
    w.write_all(&v.to_le_bytes())
}
fn write_u32(w: &mut impl Write, v: u32) -> std::io::Result<()> {
    w.write_all(&v.to_le_bytes())
}
fn write_u64(w: &mut impl Write, v: u64) -> std::io::Result<()> {
    w.write_all(&v.to_le_bytes())
}
fn write_i32(w: &mut impl Write, v: i32) -> std::io::Result<()> {
    w.write_all(&v.to_le_bytes())
}
fn write_i64(w: &mut impl Write, v: i64) -> std::io::Result<()> {
    w.write_all(&v.to_le_bytes())
}

pub fn write_header(w: &mut impl Write, tsc_hz: u64, start_wall_ns: u64, start_mono_ns: u64) -> std::io::Result<()> {
    w.write_all(MAGIC)?;
    write_u16(w, FILE_VERSION)?;
    write_u16(w, 0)?;
    write_u64(w, tsc_hz)?;
    write_u64(w, start_wall_ns)?;
    write_u64(w, start_mono_ns)?;
    /* proto identity: flowrawtf/mev-protos searcher sha prefix */
    let mut rest = [0u8; 128 - 8 - 2 - 2 - 8 - 8 - 8];
    rest[..8].copy_from_slice(b"mevproto");
    w.write_all(&rest)?;
    Ok(())
}

fn rec_payload_len(r: &Rec) -> u32 {
    /* after type+nbytes */
    8 + 8 + 8 + 8 + 4 + 8 + 4 + 8 + 4 + 4 + 4 + 2 + 2 + 64
        + r.tx.len() as u32
        + r.addr.len() as u32
}

pub fn write_rec(w: &mut impl Write, r: &Rec) -> std::io::Result<u64> {
    let typ = if r.first { REC_FIRST } else { REC_DUP };
    let nbytes = rec_payload_len(r);
    write_u32(w, typ)?;
    write_u32(w, nbytes)?;
    write_u64(w, r.t_mono_ns)?;
    write_u64(w, r.t_tsc)?;
    write_u64(w, r.wall_utc_ns)?;
    write_i64(w, r.server_side_sec)?;
    write_i32(w, r.server_side_nsec)?;
    write_i64(w, r.expiration_sec)?;
    write_i32(w, r.expiration_nsec)?;
    write_u64(w, r.sender_stake)?;
    write_u32(w, r.port)?;
    write_u32(w, r.flags)?;
    write_u32(w, r.tx.len() as u32)?;
    write_u16(w, r.addr.len() as u16)?;
    write_u16(w, 0)?;
    w.write_all(&r.sig)?;
    w.write_all(&r.tx)?;
    w.write_all(&r.addr)?;
    Ok(8 + nbytes as u64)
}

pub fn recorder_thread(
    rx: Receiver<Rec>,
    paths: Paths,
    tsc_hz: u64,
    start_wall_ns: u64,
    start_mono_ns: u64,
    bytes_out: &AtomicU64,
    recs_out: &AtomicU64,
) {
    let _ = std::fs::create_dir_all(&paths.dir);
    let flw = match OpenOptions::new()
        .create(true)
        .append(true)
        .open(&paths.flw)
    {
        Ok(f) => f,
        Err(_) => return,
    };
    let idx = match OpenOptions::new()
        .create(true)
        .append(true)
        .open(&paths.idx)
    {
        Ok(f) => f,
        Err(_) => return,
    };
    let mut fw = BufWriter::with_capacity(1 << 20, flw);
    let mut iw = BufWriter::with_capacity(1 << 16, idx);
    if write_header(&mut fw, tsc_hz, start_wall_ns, start_mono_ns).is_err() {
        return;
    }
    let mut off = HEADER_LEN;
    let _ = fw.flush();
    while let Ok(r) = rx.recv() {
        let wrote = match write_rec(&mut fw, &r) {
            Ok(n) => n,
            Err(_) => break,
        };
        if r.first {
            let _ = iw.write_all(&r.sig);
            let _ = iw.write_all(&off.to_le_bytes());
            let _ = iw.write_all(&r.t_mono_ns.to_le_bytes());
            let _ = iw.write_all(&r.wall_utc_ns.to_le_bytes());
        }
        off += wrote;
        bytes_out.fetch_add(wrote, Ordering::Relaxed);
        recs_out.fetch_add(1, Ordering::Relaxed);
    }
    let _ = fw.flush();
    let _ = iw.flush();
    drop(fw);
    drop(iw);
    let _ = File::open(&paths.flw).and_then(|f| f.sync_all());
}
