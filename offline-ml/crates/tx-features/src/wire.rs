//! Zero-copy framing of a Solana transaction.
//!
//! Walks signatures, the message header, static keys, the recent blockhash,
//! compiled instructions, and v0 address-lookup descriptors. Signatures and
//! the blockhash are skipped. No pubkey is decoded to text and no signature
//! is verified.
//!
//! [`parse_v2`] additionally frames the v1 transaction format (framed-tx-v2):
//! `0x81 | header | config mask | lifetime | counts | keys | config values |
//! instruction headers | instruction accounts+data | signatures`. It produces
//! the same `Frame` / `IxMeta` / account shells, so feature derivation is shared.

use crate::fingerprint::{alt_identity, identity_hash64};
use crate::limits::{
    INDEX_NONE, MAX_ACCOUNTS, MAX_INSTRUCTIONS, MAX_LOOKUP_TABLES, ORIGIN_ALT_READONLY,
    ORIGIN_ALT_WRITABLE, ORIGIN_STATIC, V1_CONFIG_COMPUTE_UNIT_LIMIT, V1_CONFIG_HEAP_SIZE,
    V1_CONFIG_KNOWN_BITS, V1_CONFIG_LOADED_ACCOUNTS_DATA_SIZE, V1_CONFIG_PRIORITY_FEE, V1_PREFIX,
    VERSION_LEGACY, VERSION_V0, VERSION_V1,
};
use crate::shortvec::decode_short_u16;
use crate::status::Status;
use crate::view::{AccountRoleV1, IxMeta, Scratch};

#[derive(Clone, Copy, Debug)]
pub(crate) struct Frame {
    pub version: u8,
    pub signature_count: u16,
    pub required_signatures: u16,
    pub readonly_signed: u16,
    pub readonly_unsigned: u16,
    pub static_accounts: u16,
    pub static_writable: u16,
    pub static_readonly: u16,
    pub instruction_count: u16,
    pub alt_lookup_count: u16,
    pub alt_loaded_writable: u16,
    pub alt_loaded_readonly: u16,
    pub total_accounts: u16,
    /// V1-format message config. Zero for legacy and v0.
    pub config: V1Config,
}

/// Message-level config of a v1-format transaction. A value is meaningful only
/// when its mask bit is set; absent values are 0.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct V1Config {
    pub mask: u32,
    pub priority_fee: u64,
    pub compute_unit_limit: u32,
    pub loaded_accounts_data_size: u32,
    pub heap_size: u32,
}

struct Cur<'a> {
    raw: &'a [u8],
    i: usize,
}

impl<'a> Cur<'a> {
    fn new(raw: &'a [u8]) -> Self {
        Self { raw, i: 0 }
    }

    fn u8(&mut self) -> Result<u8, Status> {
        if self.i >= self.raw.len() {
            return Err(Status::Malformed);
        }
        let b = self.raw[self.i];
        self.i += 1;
        Ok(b)
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8], Status> {
        let end = self.i.checked_add(n).ok_or(Status::Malformed)?;
        if end > self.raw.len() {
            return Err(Status::Malformed);
        }
        let s = &self.raw[self.i..end];
        self.i = end;
        Ok(s)
    }

    fn skip(&mut self, n: usize) -> Result<(), Status> {
        self.take(n).map(|_| ())
    }

    fn u32_le(&mut self) -> Result<u32, Status> {
        let b = self.take(4)?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    fn u64_le(&mut self) -> Result<u64, Status> {
        let b = self.take(8)?;
        let mut lane = [0u8; 8];
        lane.copy_from_slice(b);
        Ok(u64::from_le_bytes(lane))
    }

    fn short_u16(&mut self) -> Result<usize, Status> {
        let (v, next) = decode_short_u16(self.raw, self.i)?;
        self.i = next;
        Ok(v)
    }

    fn finish(self) -> Result<(), Status> {
        if self.i != self.raw.len() {
            Err(Status::Malformed)
        } else {
            Ok(())
        }
    }
}

fn fit_u32(offset: usize) -> Result<u32, Status> {
    u32::try_from(offset).map_err(|_| Status::CapacityExceeded)
}

fn pubkey_hash(bytes: &[u8]) -> Result<u64, Status> {
    if bytes.len() != 32 {
        return Err(Status::Malformed);
    }
    Ok(identity_hash64(bytes))
}

/// True when static account `index` is writable under the message header.
pub(crate) fn static_is_writable(
    index: usize,
    required: usize,
    readonly_signed: usize,
    static_len: usize,
    readonly_unsigned: usize,
) -> bool {
    if index < required {
        index < required - readonly_signed
    } else {
        index < static_len - readonly_unsigned
    }
}

fn shell(expanded: u16) -> AccountRoleV1 {
    let mut a = AccountRoleV1::zero();
    a.expanded_index = expanded;
    a.static_index = INDEX_NONE;
    a.alt_table_ordinal = INDEX_NONE;
    a.first_instruction_seen = INDEX_NONE;
    a.last_instruction_seen = INDEX_NONE;
    a
}

/// Parse `raw` into scratch instruction metadata and account role shells.
///
/// Capacity is decided as soon as a count is known, before that payload is
/// consumed. A count that fits, followed by too few bytes, is `Malformed`.
///
/// framed-tx-v1 (this function) expands lookup-loaded accounts table by table:
/// `T0.writable, T0.readonly, T1.writable, ...`. That deviates from the runtime
/// (and the V1 spec text) for transactions with two or more lookup tables and is
/// kept as a documented erratum because framed-tx-v1 is frozen. framed-tx-v2
/// uses [`parse_runtime_order`].
pub(crate) fn parse(raw: &[u8], scratch: &mut Scratch, accounts: &mut [AccountRoleV1]) -> Result<Frame, Status> {
    parse_legacy_v0(raw, scratch, accounts, false)
}

/// Legacy/v0 framing with the runtime's loaded-address order: every table's
/// writable indexes in table order, then every table's readonly indexes.
pub(crate) fn parse_runtime_order(raw: &[u8], scratch: &mut Scratch, accounts: &mut [AccountRoleV1]) -> Result<Frame, Status> {
    parse_legacy_v0(raw, scratch, accounts, true)
}

fn alt_row(exp: usize, origin: u8, ordinal: usize, addr: u8, table_off: usize, table_hash: u64) -> Result<AccountRoleV1, Status> {
    let mut a = shell(exp as u16);
    a.origin = origin;
    a.writable = u8::from(origin == ORIGIN_ALT_WRITABLE);
    a.readonly = u8::from(origin == ORIGIN_ALT_READONLY);
    a.alt_table_ordinal = ordinal as u16;
    a.alt_address_index = addr;
    a.alt_table_pubkey_offset = fit_u32(table_off)?;
    a.alt_table_pubkey_len = 32;
    a.alt_identity_hash64 = alt_identity(table_hash, ordinal as u64, addr as u64, origin as u64);
    Ok(a)
}

fn parse_legacy_v0(
    raw: &[u8],
    scratch: &mut Scratch,
    accounts: &mut [AccountRoleV1],
    runtime_alt_order: bool,
) -> Result<Frame, Status> {
    if raw.len() > u32::MAX as usize {
        return Err(Status::CapacityExceeded);
    }
    if accounts.len() < MAX_ACCOUNTS {
        return Err(Status::CapacityExceeded);
    }

    let mut c = Cur::new(raw);
    let sig_count = c.short_u16()?;
    let sig_bytes = sig_count.checked_mul(64).ok_or(Status::Malformed)?;
    c.skip(sig_bytes)?;

    let b0 = c.u8()?;
    let (version, required_u8) = if b0 & 0x80 != 0 {
        let ver = b0 & 0x7f;
        if ver != 0 {
            return Err(Status::UnsupportedVersion);
        }
        (VERSION_V0, c.u8()?)
    } else {
        (VERSION_LEGACY, b0)
    };
    let ro_s = c.u8()? as usize;
    let ro_u = c.u8()? as usize;
    let required = required_u8 as usize;

    if sig_count != required {
        return Err(Status::Malformed);
    }

    let static_len = c.short_u16()?;
    if static_len > MAX_ACCOUNTS {
        return Err(Status::CapacityExceeded);
    }
    if ro_s > required || required > static_len {
        return Err(Status::Malformed);
    }
    let unsigned = static_len - required;
    if ro_u > unsigned {
        return Err(Status::Malformed);
    }

    let keys_off = c.i;
    let key_bytes = static_len.checked_mul(32).ok_or(Status::Malformed)?;
    c.skip(key_bytes)?;

    for i in 0..static_len {
        let off = keys_off + i * 32;
        let pk = &raw[off..off + 32];
        let writable = static_is_writable(i, required, ro_s, static_len, ro_u);
        let mut a = shell(i as u16);
        a.origin = ORIGIN_STATIC;
        a.static_index = i as u16;
        a.signer = u8::from(i < required);
        a.writable = u8::from(writable);
        a.readonly = u8::from(!writable);
        a.fee_payer = u8::from(i == 0);
        a.has_static_identity = 1;
        a.static_pubkey_offset = fit_u32(off)?;
        a.static_pubkey_len = 32;
        a.identity_hash64 = pubkey_hash(pk)?;
        accounts[i] = a;
    }

    // Recent blockhash. Parsed only so the following fields can be found.
    c.skip(32)?;

    let ix_count = c.short_u16()?;
    if ix_count > MAX_INSTRUCTIONS {
        return Err(Status::CapacityExceeded);
    }
    for n in 0..ix_count {
        let program_index = c.u8()?;
        let acc_len = c.short_u16()?;
        let acc_off = c.i;
        c.skip(acc_len)?;
        let data_len = c.short_u16()?;
        let data_off = c.i;
        c.skip(data_len)?;
        scratch.ixs[n] = IxMeta {
            accounts_offset: fit_u32(acc_off)?,
            accounts_len: fit_u32(acc_len)?,
            data_offset: fit_u32(data_off)?,
            data_len: fit_u32(data_len)?,
            program_index,
            _pad: [0; 3],
        };
    }

    let mut total = static_len;
    let mut lookups = 0u16;
    let mut loaded_w = 0u16;
    let mut loaded_r = 0u16;

    if version == VERSION_V0 {
        let lookup_count = c.short_u16()?;
        if lookup_count > MAX_LOOKUP_TABLES {
            return Err(Status::CapacityExceeded);
        }
        lookups = lookup_count as u16;
        if runtime_alt_order {
            // (table offset, table hash, writable offset/len, readonly offset/len)
            let mut desc = [(0usize, 0u64, 0usize, 0usize, 0usize, 0usize); MAX_LOOKUP_TABLES];
            let mut running = total;
            for d in desc.iter_mut().take(lookup_count) {
                let table_off = c.i;
                let table_hash = pubkey_hash(c.take(32)?)?;
                let wl = c.short_u16()?;
                running = running.checked_add(wl).ok_or(Status::Malformed)?;
                if running > MAX_ACCOUNTS {
                    return Err(Status::CapacityExceeded);
                }
                let w_off = c.i;
                c.skip(wl)?;
                let rl = c.short_u16()?;
                running = running.checked_add(rl).ok_or(Status::Malformed)?;
                if running > MAX_ACCOUNTS {
                    return Err(Status::CapacityExceeded);
                }
                let r_off = c.i;
                c.skip(rl)?;
                *d = (table_off, table_hash, w_off, wl, r_off, rl);
            }
            for (ordinal, &(t_off, t_hash, w_off, wl, _, _)) in desc[..lookup_count].iter().enumerate() {
                for &addr in &raw[w_off..w_off + wl] {
                    accounts[total] = alt_row(total, ORIGIN_ALT_WRITABLE, ordinal, addr, t_off, t_hash)?;
                    total += 1;
                    loaded_w = loaded_w.saturating_add(1);
                }
            }
            for (ordinal, &(t_off, t_hash, _, _, r_off, rl)) in desc[..lookup_count].iter().enumerate() {
                for &addr in &raw[r_off..r_off + rl] {
                    accounts[total] = alt_row(total, ORIGIN_ALT_READONLY, ordinal, addr, t_off, t_hash)?;
                    total += 1;
                    loaded_r = loaded_r.saturating_add(1);
                }
            }
        }
        for ordinal in (0..lookup_count).filter(|_| !runtime_alt_order) {
            let table_off = c.i;
            let table = c.take(32)?;
            let table_hash = pubkey_hash(table)?;
            let wl = c.short_u16()?;
            if total.checked_add(wl).ok_or(Status::Malformed)? > MAX_ACCOUNTS {
                return Err(Status::CapacityExceeded);
            }
            let wbytes = c.take(wl)?;
            for &addr in wbytes {
                let exp = total;
                let mut a = shell(exp as u16);
                a.origin = ORIGIN_ALT_WRITABLE;
                a.writable = 1;
                a.readonly = 0;
                a.alt_table_ordinal = ordinal as u16;
                a.alt_address_index = addr;
                a.alt_table_pubkey_offset = fit_u32(table_off)?;
                a.alt_table_pubkey_len = 32;
                a.alt_identity_hash64 = alt_identity(table_hash, ordinal as u64, addr as u64, ORIGIN_ALT_WRITABLE as u64);
                accounts[exp] = a;
                total += 1;
                loaded_w = loaded_w.saturating_add(1);
            }
            let rl = c.short_u16()?;
            if total.checked_add(rl).ok_or(Status::Malformed)? > MAX_ACCOUNTS {
                return Err(Status::CapacityExceeded);
            }
            let rbytes = c.take(rl)?;
            for &addr in rbytes {
                let exp = total;
                let mut a = shell(exp as u16);
                a.origin = ORIGIN_ALT_READONLY;
                a.writable = 0;
                a.readonly = 1;
                a.alt_table_ordinal = ordinal as u16;
                a.alt_address_index = addr;
                a.alt_table_pubkey_offset = fit_u32(table_off)?;
                a.alt_table_pubkey_len = 32;
                a.alt_identity_hash64 = alt_identity(table_hash, ordinal as u64, addr as u64, ORIGIN_ALT_READONLY as u64);
                accounts[exp] = a;
                total += 1;
                loaded_r = loaded_r.saturating_add(1);
            }
        }
    }

    c.finish()?;

    let writable_signers = required - ro_s;
    let writable_unsigned = static_len - required - ro_u;
    let static_writable = writable_signers + writable_unsigned;
    let static_readonly = ro_s + ro_u;

    Ok(Frame {
        version,
        signature_count: sig_count as u16,
        required_signatures: required as u16,
        readonly_signed: ro_s as u16,
        readonly_unsigned: ro_u as u16,
        static_accounts: static_len as u16,
        static_writable: static_writable as u16,
        static_readonly: static_readonly as u16,
        instruction_count: ix_count as u16,
        alt_lookup_count: lookups,
        alt_loaded_writable: loaded_w,
        alt_loaded_readonly: loaded_r,
        total_accounts: total as u16,
        config: V1Config::default(),
    })
}

/// framed-tx-v2 framing. Mirrors the transaction reader's first-byte rule: a
/// clear high bit is a legacy/v0 compact-u16 signature count (framed as
/// [`parse`] but with the runtime loaded-address order), `0x81` is the v1
/// format, any other high-bit byte is an unsupported version.
pub(crate) fn parse_v2(raw: &[u8], scratch: &mut Scratch, accounts: &mut [AccountRoleV1]) -> Result<Frame, Status> {
    match raw.first() {
        Some(&b) if b & 0x80 == 0 => parse_runtime_order(raw, scratch, accounts),
        Some(&V1_PREFIX) => parse_v1_format(raw, scratch, accounts),
        Some(_) => Err(Status::UnsupportedVersion),
        None => Err(Status::Malformed),
    }
}

/// Frame a v1-format transaction. Every account is a static key (v1 has no
/// address lookup tables); instruction counts are `u8`, data lengths `u16` LE.
fn parse_v1_format(raw: &[u8], scratch: &mut Scratch, accounts: &mut [AccountRoleV1]) -> Result<Frame, Status> {
    if raw.len() > u32::MAX as usize {
        return Err(Status::CapacityExceeded);
    }
    if accounts.len() < MAX_ACCOUNTS {
        return Err(Status::CapacityExceeded);
    }
    let mut c = Cur::new(raw);
    if c.u8()? != V1_PREFIX {
        return Err(Status::UnsupportedVersion);
    }
    let required = c.u8()? as usize;
    let ro_s = c.u8()? as usize;
    let ro_u = c.u8()? as usize;
    let mask = c.u32_le()?;
    let fee_bits = mask & V1_CONFIG_PRIORITY_FEE;
    if mask & !V1_CONFIG_KNOWN_BITS != 0 || (fee_bits != 0 && fee_bits != V1_CONFIG_PRIORITY_FEE) {
        return Err(Status::Malformed);
    }
    // Lifetime specifier (recent blockhash). Not a feature.
    c.skip(32)?;
    let ix_count = c.u8()? as usize;
    let static_len = c.u8()? as usize;
    if ix_count > MAX_INSTRUCTIONS || static_len > MAX_ACCOUNTS {
        return Err(Status::CapacityExceeded);
    }
    if ro_s > required || required > static_len || ro_u > static_len - required {
        return Err(Status::Malformed);
    }

    let keys_off = c.i;
    c.skip(static_len * 32)?;
    for i in 0..static_len {
        let off = keys_off + i * 32;
        let writable = static_is_writable(i, required, ro_s, static_len, ro_u);
        let mut a = shell(i as u16);
        a.origin = ORIGIN_STATIC;
        a.static_index = i as u16;
        a.signer = u8::from(i < required);
        a.writable = u8::from(writable);
        a.readonly = u8::from(!writable);
        a.fee_payer = u8::from(i == 0);
        a.has_static_identity = 1;
        a.static_pubkey_offset = fit_u32(off)?;
        a.static_pubkey_len = 32;
        a.identity_hash64 = pubkey_hash(&raw[off..off + 32])?;
        accounts[i] = a;
    }

    let mut config = V1Config { mask, ..V1Config::default() };
    if fee_bits == V1_CONFIG_PRIORITY_FEE {
        config.priority_fee = c.u64_le()?;
    }
    if mask & V1_CONFIG_COMPUTE_UNIT_LIMIT != 0 {
        config.compute_unit_limit = c.u32_le()?;
    }
    if mask & V1_CONFIG_LOADED_ACCOUNTS_DATA_SIZE != 0 {
        config.loaded_accounts_data_size = c.u32_le()?;
    }
    if mask & V1_CONFIG_HEAP_SIZE != 0 {
        config.heap_size = c.u32_le()?;
    }

    // Fixed 4-byte instruction headers, then each instruction's accounts and data.
    let headers = c.take(ix_count * 4)?;
    for n in 0..ix_count {
        let h = &headers[n * 4..n * 4 + 4];
        let acc_len = h[1] as usize;
        let data_len = u16::from_le_bytes([h[2], h[3]]) as usize;
        let acc_off = c.i;
        c.skip(acc_len)?;
        let data_off = c.i;
        c.skip(data_len)?;
        scratch.ixs[n] = IxMeta {
            accounts_offset: fit_u32(acc_off)?,
            accounts_len: fit_u32(acc_len)?,
            data_offset: fit_u32(data_off)?,
            data_len: fit_u32(data_len)?,
            program_index: h[0],
            _pad: [0; 3],
        };
    }

    // Trailing signatures: exactly `num_required_signatures`, nothing after.
    c.skip(required * 64)?;
    c.finish()?;

    let static_writable = (required - ro_s) + (static_len - required - ro_u);
    Ok(Frame {
        version: VERSION_V1,
        signature_count: required as u16,
        required_signatures: required as u16,
        readonly_signed: ro_s as u16,
        readonly_unsigned: ro_u as u16,
        static_accounts: static_len as u16,
        static_writable: static_writable as u16,
        static_readonly: (ro_s + ro_u) as u16,
        instruction_count: ix_count as u16,
        alt_lookup_count: 0,
        alt_loaded_writable: 0,
        alt_loaded_readonly: 0,
        total_accounts: static_len as u16,
        config,
    })
}
