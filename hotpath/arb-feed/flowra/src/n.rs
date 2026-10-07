//! Common N event + first-wins race table (Flowra vs OrbitFlare).
//! Does not call hot_decide.

use crate::parse::{parse_tx, TxView, VenueBits};

pub const SRC_FLOWRA: u8 = 1;
pub const SRC_ORBITFLARE: u8 = 2;

#[derive(Clone, Debug)]
pub struct FeedN {
    pub t_rx_mono_ns: u64,
    pub t_rx_tsc: u64,
    pub wall_utc_ns: u64,
    pub source: u8,
    pub view: TxView,
}

impl FeedN {
    pub fn from_tx(
        source: u8,
        t_rx_mono_ns: u64,
        t_rx_tsc: u64,
        wall_utc_ns: u64,
        tx: &[u8],
        vote: &[u8; 32],
    ) -> Self {
        Self {
            t_rx_mono_ns,
            t_rx_tsc,
            wall_utc_ns,
            source,
            view: parse_tx(tx, vote),
        }
    }

    pub fn venue_dlmm(&self) -> bool {
        self.view.venues & VenueBits::DLMM != 0
    }
    pub fn venue_pump(&self) -> bool {
        self.view.venues & VenueBits::PUMP != 0
    }
}

#[derive(Clone, Debug)]
pub struct RaceFirst {
    pub source: u8,
    pub t_mono_ns: u64,
    pub have_n: bool,
}

#[derive(Clone, Debug)]
pub enum RaceHit {
    First,
    DupSame,
    Second { lead_ns: i64, first_source: u8 },
}

pub struct RaceTable {
    first: std::collections::HashMap<[u8; 64], RaceFirst>,
}

impl RaceTable {
    pub fn new() -> Self {
        Self {
            first: std::collections::HashMap::with_capacity(1 << 18),
        }
    }

    pub fn see(&mut self, sig: &[u8; 64], source: u8, t_mono_ns: u64, have_n: bool) -> RaceHit {
        if *sig == [0u8; 64] {
            return RaceHit::DupSame;
        }
        if let Some(e) = self.first.get(sig) {
            if e.source == source {
                return RaceHit::DupSame;
            }
            let lead = t_mono_ns as i64 - e.t_mono_ns as i64;
            return RaceHit::Second {
                lead_ns: lead,
                first_source: e.source,
            };
        }
        self.first.insert(
            *sig,
            RaceFirst {
                source,
                t_mono_ns,
                have_n,
            },
        );
        RaceHit::First
    }

    pub fn len(&self) -> usize {
        self.first.len()
    }
}
