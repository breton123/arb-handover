#![no_std]

//! Generic hop dispatcher. Supported venues today: DLMM, Pump.
//! Adding a venue later is a new adapter in the hop loop, then full
//! route closure over the new set. Does not replace OUR_EXEC 38dsYLgt.

use pinocchio::{
    account_info::AccountInfo,
    cpi::slice_invoke,
    entrypoint,
    instruction::{AccountMeta, Instruction},
    program_error::ProgramError,
    pubkey::Pubkey,
};

entrypoint!(process);

#[cfg(target_os = "solana")]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}

const DISC: [u8; 8] = *b"ARBHOPS0";
const DLMM_SWAP2: [u8; 8] = [0x41, 0x4b, 0x3f, 0x4c, 0xeb, 0x5b, 0x5b, 0x88];
const PUMP_SELL: [u8; 8] = [0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad];
const PUMP_BUY_EXACT_Q: [u8; 8] = [0xc6, 0x2e, 0x15, 0x52, 0xb4, 0xd9, 0xe8, 0x70];

const PROTO_DLMM: u8 = 1;
const PROTO_PUMP: u8 = 2;

const ACC_AUTHORITY: usize = 0;
const ACC_USER0: usize = 1;
const ACC_USER1: usize = 2;
const ACC_USER2: usize = 3;
const ACC_TOKEN_PROGRAM: usize = 4;
const ACC_TOKEN_2022: usize = 5;
const ACC_MEMO: usize = 6;
const ACC_DLMM: usize = 7;
const ACC_DLMM_EVENT: usize = 8;
const SHARED_N: usize = 9;

const PUMP_N_SELL: usize = 24;
const PUMP_N_BUY: usize = 26;
const DLMM_HOP_N: usize = 10;

const ERR_IX: u32 = 1;
const ERR_ACCOUNTS: u32 = 2;
const ERR_SIGNER: u32 = 3;
const ERR_DLMM: u32 = 4;
const ERR_PUMP: u32 = 5;
const ERR_PROFIT: u32 = 6;
const ERR_TOKEN: u32 = 7;
const ERR_HOP: u32 = 8;

const SPL_AMOUNT_OFF: usize = 64;
const IX_LEN: usize = 40;

fn err(c: u32) -> ProgramError {
    ProgramError::Custom(c)
}

fn load_u64(p: &[u8]) -> u64 {
    let mut b = [0u8; 8];
    b.copy_from_slice(p);
    u64::from_le_bytes(b)
}

fn tok_amt(a: &AccountInfo) -> Result<u64, ProgramError> {
    let d = a.try_borrow_data().map_err(|_| err(ERR_TOKEN))?;
    if d.len() < SPL_AMOUNT_OFF + 8 {
        return Err(err(ERR_TOKEN));
    }
    Ok(load_u64(&d[SPL_AMOUNT_OFF..SPL_AMOUNT_OFF + 8]))
}

fn user_ata(acc: &[AccountInfo], i: u8) -> Result<&AccountInfo, ProgramError> {
    match i {
        0 => Ok(&acc[ACC_USER0]),
        1 => Ok(&acc[ACC_USER1]),
        2 => Ok(&acc[ACC_USER2]),
        _ => Err(err(ERR_IX)),
    }
}

fn meta<'a>(a: &'a AccountInfo, w: bool, s: bool) -> AccountMeta<'a> {
    AccountMeta {
        pubkey: a.key(),
        is_writable: w,
        is_signer: s,
    }
}

fn pump_writable_sell(i: usize) -> bool {
    matches!(i, 0 | 1 | 5 | 6 | 7 | 8 | 10 | 17 | 23)
}

fn pump_writable_buy(i: usize) -> bool {
    matches!(i, 0 | 1 | 5 | 6 | 7 | 8 | 10 | 17 | 19 | 20 | 25)
}

/// Hop slice: pair, bitmap, rx, ry, oracle, host, mx, my, bin0, bin1.
fn cpi_dlmm(
    acc: &[AccountInfo],
    hop: usize,
    amount_in: u64,
    user_in: &AccountInfo,
    user_out: &AccountInfo,
) -> Result<(), ProgramError> {
    if hop + DLMM_HOP_N > acc.len() {
        return Err(err(ERR_ACCOUNTS));
    }
    let mut data = [0u8; 28];
    data[..8].copy_from_slice(&DLMM_SWAP2);
    data[8..16].copy_from_slice(&amount_in.to_le_bytes());
    let metas = [
        meta(&acc[hop], true, false),
        meta(&acc[hop + 1], false, false),
        meta(&acc[hop + 2], true, false),
        meta(&acc[hop + 3], true, false),
        meta(user_in, true, false),
        meta(user_out, true, false),
        meta(&acc[hop + 6], false, false),
        meta(&acc[hop + 7], false, false),
        meta(&acc[hop + 4], true, false),
        meta(&acc[hop + 5], false, false),
        meta(&acc[ACC_AUTHORITY], true, true),
        meta(&acc[ACC_TOKEN_2022], false, false),
        meta(&acc[ACC_TOKEN_PROGRAM], false, false),
        meta(&acc[ACC_MEMO], false, false),
        meta(&acc[ACC_DLMM_EVENT], false, false),
        meta(&acc[ACC_DLMM], false, false),
        meta(&acc[hop + 8], true, false),
        meta(&acc[hop + 9], true, false),
    ];
    let infos = [
        &acc[hop],
        &acc[hop + 1],
        &acc[hop + 2],
        &acc[hop + 3],
        user_in,
        user_out,
        &acc[hop + 6],
        &acc[hop + 7],
        &acc[hop + 4],
        &acc[hop + 5],
        &acc[ACC_AUTHORITY],
        &acc[ACC_TOKEN_2022],
        &acc[ACC_TOKEN_PROGRAM],
        &acc[ACC_MEMO],
        &acc[ACC_DLMM_EVENT],
        &acc[ACC_DLMM],
        &acc[hop + 8],
        &acc[hop + 9],
    ];
    let ix = Instruction {
        program_id: acc[ACC_DLMM].key(),
        accounts: &metas,
        data: &data,
    };
    slice_invoke(&ix, &infos).map_err(|_| err(ERR_DLMM))
}

fn cpi_pump_sell(acc: &[AccountInfo], hop: usize, amount_in: u64) -> Result<(), ProgramError> {
    if hop + PUMP_N_SELL > acc.len() {
        return Err(err(ERR_ACCOUNTS));
    }
    let pump = &acc[hop..hop + PUMP_N_SELL];
    let mut data = [0u8; 24];
    data[..8].copy_from_slice(&PUMP_SELL);
    data[8..16].copy_from_slice(&amount_in.to_le_bytes());
    let auth = acc[ACC_AUTHORITY].key();
    let metas: [AccountMeta; PUMP_N_SELL] = core::array::from_fn(|i| {
        meta(&pump[i], pump_writable_sell(i), pump[i].key() == auth)
    });
    let infos: [&AccountInfo; PUMP_N_SELL] = [
        &pump[0], &pump[1], &pump[2], &pump[3], &pump[4], &pump[5], &pump[6],
        &pump[7], &pump[8], &pump[9], &pump[10], &pump[11], &pump[12], &pump[13],
        &pump[14], &pump[15], &pump[16], &pump[17], &pump[18], &pump[19],
        &pump[20], &pump[21], &pump[22], &pump[23],
    ];
    let ix = Instruction {
        program_id: pump[16].key(),
        accounts: &metas,
        data: &data,
    };
    slice_invoke(&ix, &infos).map_err(|_| err(ERR_PUMP))
}

fn cpi_pump_buy(acc: &[AccountInfo], hop: usize, amount_in: u64) -> Result<(), ProgramError> {
    if hop + PUMP_N_BUY > acc.len() {
        return Err(err(ERR_ACCOUNTS));
    }
    let pump = &acc[hop..hop + PUMP_N_BUY];
    let mut data = [0u8; 24];
    data[..8].copy_from_slice(&PUMP_BUY_EXACT_Q);
    data[8..16].copy_from_slice(&amount_in.to_le_bytes());
    data[16..24].copy_from_slice(&1u64.to_le_bytes());
    let auth = acc[ACC_AUTHORITY].key();
    let metas: [AccountMeta; PUMP_N_BUY] = core::array::from_fn(|i| {
        meta(&pump[i], pump_writable_buy(i), pump[i].key() == auth)
    });
    let infos: [&AccountInfo; PUMP_N_BUY] = [
        &pump[0], &pump[1], &pump[2], &pump[3], &pump[4], &pump[5], &pump[6],
        &pump[7], &pump[8], &pump[9], &pump[10], &pump[11], &pump[12], &pump[13],
        &pump[14], &pump[15], &pump[16], &pump[17], &pump[18], &pump[19],
        &pump[20], &pump[21], &pump[22], &pump[23], &pump[24], &pump[25],
    ];
    let ix = Instruction {
        program_id: pump[16].key(),
        accounts: &metas,
        data: &data,
    };
    slice_invoke(&ix, &infos).map_err(|_| err(ERR_PUMP))
}

fn process(_pid: &Pubkey, accounts: &[AccountInfo], data: &[u8]) -> Result<(), ProgramError> {
    if data.len() != IX_LEN || data[..8] != DISC {
        return Err(err(ERR_IX));
    }
    if accounts.len() < SHARED_N {
        return Err(err(ERR_ACCOUNTS));
    }
    if !accounts[ACC_AUTHORITY].is_signer() {
        return Err(err(ERR_SIGNER));
    }
    let hop_count = data[8];
    if hop_count < 2 || hop_count > 3 {
        return Err(err(ERR_IX));
    }
    let mut amount = load_u64(&data[9..17]);
    let min_profit = load_u64(&data[17..25]);
    if amount == 0 {
        return Err(err(ERR_IX));
    }

    let q0 = tok_amt(&accounts[ACC_USER0])?;
    let mut off = SHARED_N;
    let mut i = 0u8;
    while i < hop_count {
        let proto = data[25 + (i as usize) * 3];
        let ina = data[26 + (i as usize) * 3];
        let outa = data[27 + (i as usize) * 3];
        let n = data[34 + i as usize] as usize;
        if off + n > accounts.len() {
            return Err(err(ERR_ACCOUNTS));
        }
        let user_in = user_ata(accounts, ina)?;
        let user_out = user_ata(accounts, outa)?;
        let before = tok_amt(user_out)?;
        if proto == PROTO_DLMM {
            if n != DLMM_HOP_N {
                return Err(err(ERR_HOP));
            }
            cpi_dlmm(accounts, off, amount, user_in, user_out)?;
        } else if proto == PROTO_PUMP {
            if n == PUMP_N_BUY {
                cpi_pump_buy(accounts, off, amount)?;
            } else if n == PUMP_N_SELL {
                cpi_pump_sell(accounts, off, amount)?;
            } else {
                return Err(err(ERR_HOP));
            }
        } else {
            /* Unsupported venue. Do not silently skip. */
            return Err(err(ERR_HOP));
        }
        let after = tok_amt(user_out)?;
        amount = after.saturating_sub(before);
        if amount == 0 {
            return Err(err(ERR_HOP));
        }
        off += n;
        i += 1;
    }

    let q1 = tok_amt(&accounts[ACC_USER0])?;
    if q1 < q0 || q1 - q0 < min_profit {
        return Err(err(ERR_PROFIT));
    }
    Ok(())
}
