//! Off-timestamp Solana wire parse + six-venue classify + diagnostic trigger decode.
//! Does not call hot_decide. Matches hot_decode_trigger variants only.

use std::collections::HashMap;

pub const TX_SIG_SZ: usize = 64;
pub const TX_KEY_SZ: usize = 32;
pub const TX_SIG_MAX: usize = 12;
pub const TX_KEY_MAX: usize = 128;
pub const TX_INSTR_MAX: usize = 64;

/* Meteora DLMM  LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo */
pub const PROG_DLMM: [u8; 32] = [
    0x04, 0xe9, 0xe1, 0x2f, 0xbc, 0x84, 0xe8, 0x26, 0xc9, 0x32, 0xcc, 0xe9, 0xe2, 0x64, 0x0c,
    0xce, 0x15, 0x59, 0x0c, 0x1c, 0x62, 0x73, 0xb0, 0x92, 0x57, 0x08, 0xba, 0x3b, 0x85, 0x20,
    0xb0, 0xbc,
];
/* PumpSwap  pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA */
pub const PROG_PUMP: [u8; 32] = [
    0x0c, 0x14, 0xde, 0xfc, 0x82, 0x5e, 0xc6, 0x76, 0x94, 0x25, 0x08, 0x18, 0xbb, 0x65, 0x40,
    0x65, 0xf4, 0x29, 0x8d, 0x31, 0x56, 0xd5, 0x71, 0xb4, 0xd4, 0xf8, 0x09, 0x0c, 0x18, 0xe9,
    0xa8, 0x63,
];
/* Raydium CLMM  CAMMCzo5YL8w4VFF8KVHrK22GGUsp5VTaW7grrKgrWqK */
pub const PROG_CLMM: [u8; 32] = [
    0xa5, 0xd5, 0xca, 0x9e, 0x04, 0xcf, 0x5d, 0xb5, 0x90, 0xb7, 0x14, 0xba, 0x2f, 0xe3, 0x2c,
    0xb1, 0x59, 0x13, 0x3f, 0xc1, 0xc1, 0x92, 0xb7, 0x22, 0x57, 0xfd, 0x07, 0xd3, 0x9c, 0xb0,
    0x40, 0x1e,
];
/* Raydium CPMM  CPMMoo8L3F4NbTegBCKVNunggL7H1ZpdTHKxQB5qKP1C */
pub const PROG_CPMM: [u8; 32] = [
    0xa9, 0x2a, 0x5a, 0x8b, 0x4f, 0x29, 0x59, 0x52, 0x84, 0x25, 0x50, 0xaa, 0x93, 0xfd, 0x5b,
    0x95, 0xb5, 0xac, 0xe6, 0xa8, 0xeb, 0x92, 0x0c, 0x93, 0x94, 0x2e, 0x43, 0x69, 0x0c, 0x20,
    0xec, 0x73,
];
/* Meteora DAMM v2  cpamdpZCGKUy5JxQXB4dcpGPiikHawvSWAd6mEn1sGG */
pub const PROG_DAMM: [u8; 32] = [
    0x09, 0x2d, 0x21, 0x35, 0x65, 0x7a, 0x15, 0x9c, 0x2b, 0x87, 0xd4, 0xb6, 0x6a, 0x70, 0xdb,
    0x8e, 0x97, 0x52, 0x38, 0x9f, 0xf7, 0x6a, 0xaf, 0x20, 0x6c, 0xed, 0x06, 0x3a, 0x38, 0xf9,
    0x5a, 0xed,
];
/* Orca Whirlpool  whirLbMiicVdio4qvUfM5KAg6Ct8VwpYzGff3uctyCc */
pub const PROG_ORCA: [u8; 32] = [
    0x0e, 0x03, 0x68, 0x5f, 0x8e, 0x90, 0x90, 0x53, 0xe4, 0x58, 0x12, 0x1c, 0x66, 0xf5, 0xa7,
    0x6a, 0xed, 0xc7, 0x70, 0x6a, 0xa1, 0x1c, 0x82, 0xf8, 0xaa, 0x95, 0x2a, 0x8f, 0x2b, 0x78,
    0x79, 0xa9,
];

const DLMM_SWAP2: [u8; 8] = [0x41, 0x4b, 0x3f, 0x4c, 0xeb, 0x5b, 0x5b, 0x88];
const PUMP_BUY: [u8; 8] = [0x66, 0x06, 0x3d, 0x12, 0x01, 0xda, 0xeb, 0xea];
const PUMP_SELL: [u8; 8] = [0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad];

#[derive(Clone, Copy, Debug)]
pub struct TxView {
    pub valid: bool,
    pub version: i8, /* -1 invalid, 0 legacy, 0x00 v0, 0x01 v1 */
    pub nsig: u8,
    pub nkeys: u16,
    pub ninstr: u16,
    pub has_alt: bool,
    pub vote: bool,
    pub venues: u8,
    pub trigger_ok: bool,
    pub protocol: u8, /* 1 DLMM 2 Pump when trigger_ok */
    pub amount_in: u64,
    pub min_out: u64,
    pub direction: u8,
    pub pool: [u8; 32],
    pub sig: [u8; 64],
}

#[derive(Clone, Copy, Debug, Default)]
pub struct VenueBits;

impl VenueBits {
    pub const DLMM: u8 = 1 << 0;
    pub const PUMP: u8 = 1 << 1;
    pub const DAMM: u8 = 1 << 2;
    pub const CLMM: u8 = 1 << 3;
    pub const CPMM: u8 = 1 << 4;
    pub const ORCA: u8 = 1 << 5;
}

pub fn cu16(p: &[u8]) -> Option<(u16, usize)> {
    if p.is_empty() {
        return None;
    }
    if p[0] & 0x80 == 0 {
        return Some((p[0] as u16, 1));
    }
    if p.len() < 2 {
        return None;
    }
    if p[1] & 0x80 == 0 {
        if p[1] == 0 {
            return None;
        }
        return Some(((p[0] as u16 & 0x7f) | ((p[1] as u16) << 7), 2));
    }
    if p.len() < 3 || (p[2] & 0xfc) != 0 || p[2] == 0 {
        return None;
    }
    Some((
        (p[0] as u16 & 0x7f) | ((p[1] as u16 & 0x7f) << 7) | ((p[2] as u16) << 14),
        3,
    ))
}

/// First signature only. Does not walk the message.
pub fn extract_sig(tx: &[u8]) -> Option<[u8; 64]> {
    if tx.is_empty() || (tx[0] & 0x80) != 0 {
        return None;
    }
    let nsig = tx[0] as usize;
    if nsig < 1 || nsig > TX_SIG_MAX {
        return None;
    }
    if tx.len() < 1 + nsig * TX_SIG_SZ {
        return None;
    }
    let mut sig = [0u8; 64];
    sig.copy_from_slice(&tx[1..1 + TX_SIG_SZ]);
    Some(sig)
}

fn venue_bit(key: &[u8]) -> u8 {
    if key == PROG_DLMM {
        VenueBits::DLMM
    } else if key == PROG_PUMP {
        VenueBits::PUMP
    } else if key == PROG_DAMM {
        VenueBits::DAMM
    } else if key == PROG_CLMM {
        VenueBits::CLMM
    } else if key == PROG_CPMM {
        VenueBits::CPMM
    } else if key == PROG_ORCA {
        VenueBits::ORCA
    } else {
        0
    }
}

fn trigger_from_ix(
    prog: &[u8],
    data: &[u8],
    first_acct: Option<[u8; 32]>,
) -> Option<(u8, u64, u64, u8, [u8; 32])> {
    if data.len() < 24 {
        return None;
    }
    let ain = u64::from_le_bytes(data[8..16].try_into().ok()?);
    let mino = u64::from_le_bytes(data[16..24].try_into().ok()?);
    let pool = first_acct.unwrap_or([0u8; 32]);
    if prog == PROG_DLMM && data[..8] == DLMM_SWAP2 {
        return Some((1, ain, mino, 0, pool));
    }
    if prog == PROG_PUMP && data[..8] == PUMP_BUY {
        return Some((2, ain, mino, 0, pool));
    }
    if prog == PROG_PUMP && data[..8] == PUMP_SELL {
        return Some((2, ain, mino, 1, pool));
    }
    None
}

pub fn parse_tx(tx: &[u8], vote_prog: &[u8; 32]) -> TxView {
    let mut v = TxView {
        valid: false,
        version: -1,
        nsig: 0,
        nkeys: 0,
        ninstr: 0,
        has_alt: false,
        vote: false,
        venues: 0,
        trigger_ok: false,
        protocol: 0,
        amount_in: 0,
        min_out: 0,
        direction: 0,
        pool: [0u8; 32],
        sig: [0u8; 64],
    };
    if let Some(sig) = extract_sig(tx) {
        v.sig = sig;
    } else {
        return v;
    }
    if tx.is_empty() || (tx[0] & 0x80) != 0 {
        return v;
    }
    let nsig = tx[0] as usize;
    v.nsig = nsig as u8;
    let mut i = 1 + nsig * TX_SIG_SZ;
    if i >= tx.len() {
        return v;
    }
    let b0 = tx[i];
    i += 1;
    let mut versioned = false;
    let mut ver: i8 = 0;
    if (b0 & 0x80) != 0 {
        ver = (b0 & 0x7f) as i8;
        versioned = true;
        if i >= tx.len() || tx[i] != nsig as u8 {
            return v;
        }
        i += 1;
    } else if b0 != nsig as u8 {
        return v;
    }
    if i + 2 > tx.len() {
        return v;
    }
    let ro_s = tx[i];
    i += 1;
    let ro_u = tx[i];
    i += 1;
    let (nkeys, used) = match cu16(&tx[i..]) {
        Some(x) => x,
        None => return v,
    };
    i += used;
    if nsig < 1
        || nsig > TX_SIG_MAX
        || (ro_s as usize) >= nsig
        || nkeys < nsig as u16
        || nkeys as usize > TX_KEY_MAX
        || nsig as u16 + ro_u as u16 > nkeys
    {
        return v;
    }
    v.nkeys = nkeys;
    let keys_off = i;
    if i + 32 * nkeys as usize + 32 > tx.len() {
        return v;
    }
    let keys = &tx[keys_off..keys_off + 32 * nkeys as usize];
    i += 32 * nkeys as usize + 32;
    let (ninstr, used) = match cu16(&tx[i..]) {
        Some(x) => x,
        None => return v,
    };
    i += used;
    if ninstr == 0 || ninstr as usize > TX_INSTR_MAX {
        return v;
    }
    v.ninstr = ninstr;
    let mut venues = 0u8;
    let mut vote = false;
    let mut trigger_ok = false;
    let mut ain = 0u64;
    let mut mino = 0u64;
    let mut dir = 0u8;
    let mut pool = [0u8; 32];
    for _ in 0..ninstr {
        if i >= tx.len() {
            return v;
        }
        let prog_idx = tx[i] as usize;
        i += 1;
        if prog_idx >= nkeys as usize {
            return v;
        }
        let (nacct, used) = match cu16(&tx[i..]) {
            Some(x) => x,
            None => return v,
        };
        i += used;
        if i + nacct as usize > tx.len() {
            return v;
        }
        let accs = &tx[i..i + nacct as usize];
        i += nacct as usize;
        let (dlen, used) = match cu16(&tx[i..]) {
            Some(x) => x,
            None => return v,
        };
        i += used;
        if i + dlen as usize > tx.len() {
            return v;
        }
        let data = &tx[i..i + dlen as usize];
        i += dlen as usize;
        let prog = &keys[prog_idx * 32..prog_idx * 32 + 32];
        venues |= venue_bit(prog);
        if prog == vote_prog {
            vote = true;
        }
        if !trigger_ok {
            let first = if !accs.is_empty() && (accs[0] as usize) < nkeys as usize {
                let mut p = [0u8; 32];
                let o = accs[0] as usize * 32;
                p.copy_from_slice(&keys[o..o + 32]);
                Some(p)
            } else {
                None
            };
            if let Some((pr, a, b, d, po)) = trigger_from_ix(prog, data, first) {
                trigger_ok = true;
                v.protocol = pr;
                ain = a;
                mino = b;
                dir = d;
                pool = po;
            }
        }
    }
    let mut has_alt = false;
    if versioned && i < tx.len() {
        if let Some((nlut, _)) = cu16(&tx[i..]) {
            has_alt = nlut > 0;
        }
    }
    for k in 0..nkeys as usize {
        venues |= venue_bit(&keys[k * 32..k * 32 + 32]);
        if &keys[k * 32..k * 32 + 32] == vote_prog {
            vote = true;
        }
    }
    v.valid = true;
    v.version = if versioned { ver } else { -2 }; /* -2 = legacy */
    v.has_alt = has_alt;
    v.vote = vote;
    v.venues = venues;
    v.trigger_ok = trigger_ok;
    v.amount_in = ain;
    v.min_out = mino;
    v.direction = dir;
    v.pool = pool;
    v
}

pub struct Dedup {
    first: HashMap<[u8; 64], DedupEnt>,
}

pub struct DedupEnt {
    pub first_mono_ns: u64,
    pub first_tsc: u64,
    pub first_wall_ns: u64,
    pub dups: u32,
}

impl Dedup {
    pub fn new() -> Self {
        Self {
            first: HashMap::with_capacity(1 << 18),
        }
    }

    pub fn see(&mut self, sig: &[u8; 64], mono: u64, tsc: u64, wall: u64) -> bool {
        if let Some(e) = self.first.get_mut(sig) {
            e.dups = e.dups.saturating_add(1);
            false
        } else {
            self.first.insert(
                *sig,
                DedupEnt {
                    first_mono_ns: mono,
                    first_tsc: tsc,
                    first_wall_ns: wall,
                    dups: 0,
                },
            );
            true
        }
    }

    pub fn unique(&self) -> u64 {
        self.first.len() as u64
    }
}
