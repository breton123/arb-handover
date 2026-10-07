//! FLOWRA1 capture reader. Join later by sig against OrbitFlare.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use crate::record::{HEADER_LEN, MAGIC, REC_DUP, REC_FIRST};

#[derive(Clone, Debug)]
pub struct FlwRec {
    pub first: bool,
    pub t_mono_ns: u64,
    pub t_tsc: u64,
    pub wall_utc_ns: u64,
    pub sig: [u8; 64],
    pub tx: Vec<u8>,
    pub addr: Vec<u8>,
}

pub fn read_u16(p: &[u8]) -> u16 {
    u16::from_le_bytes([p[0], p[1]])
}
pub fn read_u32(p: &[u8]) -> u32 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]])
}
pub fn read_u64(p: &[u8]) -> u64 {
    u64::from_le_bytes([p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]])
}

pub fn open_flw(path: &Path) -> Result<File, String> {
    let mut f = File::open(path).map_err(|e| e.to_string())?;
    let mut hdr = [0u8; HEADER_LEN as usize];
    f.read_exact(&mut hdr).map_err(|e| e.to_string())?;
    if &hdr[..8] != MAGIC {
        return Err("bad FLOWRA1 magic".into());
    }
    Ok(f)
}

pub fn read_rec(f: &mut File) -> Result<Option<FlwRec>, String> {
    let mut pref = [0u8; 8];
    match f.read_exact(&mut pref) {
        Ok(()) => {}
        Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(e) => return Err(e.to_string()),
    }
    let typ = read_u32(&pref[0..4]);
    let nbytes = read_u32(&pref[4..8]) as usize;
    if nbytes < 136 || nbytes > 8 * 1024 {
        return Err(format!("bad rec nbytes {nbytes}"));
    }
    let mut body = vec![0u8; nbytes];
    f.read_exact(&mut body).map_err(|e| e.to_string())?;
    let t_mono = read_u64(&body[0..8]);
    let t_tsc = read_u64(&body[8..16]);
    let wall = read_u64(&body[16..24]);
    let tx_len = read_u32(&body[64..68]) as usize;
    let addr_len = read_u16(&body[68..70]) as usize;
    if 72 + 64 + tx_len + addr_len != nbytes {
        return Err("rec length mismatch".into());
    }
    let mut sig = [0u8; 64];
    sig.copy_from_slice(&body[72..136]);
    let tx_off = 136;
    let tx = body[tx_off..tx_off + tx_len].to_vec();
    let addr = body[tx_off + tx_len..tx_off + tx_len + addr_len].to_vec();
    Ok(Some(FlwRec {
        first: typ == REC_FIRST,
        t_mono_ns: t_mono,
        t_tsc,
        wall_utc_ns: wall,
        sig,
        tx,
        addr,
    }))
}

pub fn skip_header_at(f: &mut File) -> Result<(), String> {
    f.seek(SeekFrom::Start(HEADER_LEN)).map_err(|e| e.to_string())?;
    Ok(())
}

#[allow(dead_code)]
pub fn rec_dup_type() -> u32 {
    REC_DUP
}
