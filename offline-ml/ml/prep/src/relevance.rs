//! `relevance-v1`: can this raw transaction touch a tracked market?
//!
//! Deterministic and raw-only. It reads the static account keys that are on the
//! wire (via the offsets the framed-tx encoder already produced; `enc` is the
//! framed-tx-v1-layout `EncodedTxV2::base`) and compares
//! them with the tracked universe. ALT-loaded addresses are not on the wire and
//! are not resolved here. No labels, logs, inner instructions or account state.
//!
//! The tracked universe is the labeller's admitted markets: venue programs from
//! `markets.parquet`, plus every market-specific account in
//! `execution_catalog.parquet` (pool state, vaults, tick/bin arrays and bitmaps,
//! observation, oracle). Accounts shared across markets (mints, token program,
//! clock, configs, event authority, program data) are not market-specific and are
//! excluded; they would mark nearly every transaction relevant.

use std::collections::{BTreeSet, HashSet};
use std::path::Path;

use sha2::{Digest, Sha256};
use tx_features::{EncodedTxV1, ORIGIN_STATIC};

use crate::pq::{self, Cols};
use crate::Result;

pub const RELEVANCE_SCHEMA: &str = "relevance-v1";

/// `execution_catalog.role` values that identify one market's own accounts.
pub const MARKET_ROLES: &[&str] =
    &["POOL_STATE", "VAULT", "TICK_ARRAY", "BIN_ARRAY", "BIN_BITMAP", "TICK_BITMAP", "OBSERVATION", "ORACLE"];

pub struct TrackedUniverse {
    market_accounts: HashSet<[u8; 32]>,
    venue_programs: HashSet<[u8; 32]>,
    pub markets: usize,
    pub digest: String,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Relevance {
    /// Static keys that are tracked market accounts and writable in this transaction.
    pub market_writable: u16,
    /// Static keys that are tracked market accounts and read-only in this transaction.
    pub market_readonly: u16,
    /// Static keys that are a tracked venue program (invoked or passed for CPI).
    pub venue_program: u16,
}

impl Relevance {
    /// The gate. A tracked market account or venue program is named directly on the wire.
    pub fn relevant(&self) -> bool {
        self.market_writable > 0 || self.market_readonly > 0 || self.venue_program > 0
    }
}

fn key(b58: &str) -> Result<[u8; 32]> {
    let v = bs58::decode(b58).into_vec().map_err(|e| format!("bad pubkey {b58}: {e}"))?;
    v.try_into().map_err(|_| format!("pubkey {b58} is not 32 bytes").into())
}

impl TrackedUniverse {
    pub fn load(markets: &Path, execution_catalog: &Path) -> Result<Self> {
        let mut programs = BTreeSet::new();
        let mut pools = BTreeSet::new();
        let (reader, order) = pq::open(markets, &["pool", "program"])?;
        for batch in reader {
            let batch = batch?;
            let c = Cols { batch: &batch, order: &order };
            for i in 0..batch.num_rows() {
                pools.insert(c.str(0)?.value(i).to_string());
                programs.insert(c.str(1)?.value(i).to_string());
            }
        }
        let mut accounts = BTreeSet::new();
        let (reader, order) = pq::open(execution_catalog, &["pool", "role", "pubkey"])?;
        for batch in reader {
            let batch = batch?;
            let c = Cols { batch: &batch, order: &order };
            for i in 0..batch.num_rows() {
                let pubkey = c.str(2)?.value(i);
                if !pubkey.is_empty() && pools.contains(c.str(0)?.value(i)) && MARKET_ROLES.contains(&c.str(1)?.value(i)) {
                    accounts.insert(pubkey.to_string());
                }
            }
        }
        // Pools are market state even if the catalog omitted them.
        accounts.extend(pools.iter().cloned());
        let mut h = Sha256::new();
        h.update(RELEVANCE_SCHEMA.as_bytes());
        for p in &programs {
            h.update(b"P");
            h.update(p.as_bytes());
        }
        for a in &accounts {
            h.update(b"A");
            h.update(a.as_bytes());
        }
        Ok(Self {
            market_accounts: accounts.iter().map(|a| key(a)).collect::<Result<_>>()?,
            venue_programs: programs.iter().map(|p| key(p)).collect::<Result<_>>()?,
            markets: pools.len(),
            digest: crate::features::hex(&h.finalize()),
        })
    }

    pub fn market_account_count(&self) -> usize {
        self.market_accounts.len()
    }

    /// `enc` must be the `Status::Ok` encoding of `raw`.
    pub fn relevance(&self, enc: &EncodedTxV1, raw: &[u8]) -> Relevance {
        let mut r = Relevance::default();
        for acct in &enc.accounts[..enc.header.total_account_reference_count as usize] {
            if acct.origin != ORIGIN_STATIC || acct.static_pubkey_len != 32 {
                continue;
            }
            let off = acct.static_pubkey_offset as usize;
            let Ok(k) = <[u8; 32]>::try_from(&raw[off..off + 32]) else { continue };
            if self.market_accounts.contains(&k) {
                if acct.writable == 1 {
                    r.market_writable += 1;
                } else {
                    r.market_readonly += 1;
                }
            }
            if self.venue_programs.contains(&k) {
                r.venue_program += 1;
            }
        }
        r
    }
}
