#![no_std]

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

const DISC: [u8; 8] = *b"ARBDLMM2";
const DLMM_SWAP2: [u8; 8] = [0x41, 0x4b, 0x3f, 0x4c, 0xeb, 0x5b, 0x5b, 0x88];

const ACC_AUTHORITY: usize = 0;
const ACC_USER_QUOTE: usize = 1;
const ACC_USER_BASE: usize = 2;
const ACC_TOKEN_PROGRAM: usize = 3;
const ACC_TOKEN_2022: usize = 4;
const ACC_MEMO: usize = 5;
const ACC_DLMM: usize = 6;
const ACC_DLMM_EVENT: usize = 7;
const HOP0: usize = 8;
const HOP1: usize = 18;
const HOP_N: usize = 10;
const N_ACC: usize = 28;

const ERR_IX: u32 = 1;
const ERR_ACCOUNTS: u32 = 2;
const ERR_SIGNER: u32 = 3;
const ERR_DLMM: u32 = 4;
const ERR_PROFIT: u32 = 6;
const ERR_TOKEN: u32 = 7;

const SPL_AMOUNT_OFF: usize = 64;

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

fn meta<'a>(a: &'a AccountInfo, w: bool, s: bool) -> AccountMeta<'a> {
    AccountMeta {
        pubkey: a.key(),
        is_writable: w,
        is_signer: s,
    }
}

/// Hop slice: pair, bitmap, rx, ry, oracle, host, mx, my, bin0, bin1.
#[inline(never)]
fn cpi_dlmm_hop(
    acc: &[AccountInfo],
    hop: usize,
    amount_in: u64,
    token_in_is_quote: bool,
) -> Result<(), ProgramError> {
    let base = hop;
    if base + HOP_N > acc.len() {
        return Err(err(ERR_ACCOUNTS));
    }
    let (user_in, user_out): (&AccountInfo, &AccountInfo) = if token_in_is_quote {
        (&acc[ACC_USER_QUOTE], &acc[ACC_USER_BASE])
    } else {
        (&acc[ACC_USER_BASE], &acc[ACC_USER_QUOTE])
    };
    let mut data = [0u8; 28];
    data[..8].copy_from_slice(&DLMM_SWAP2);
    data[8..16].copy_from_slice(&amount_in.to_le_bytes());

    let metas = [
        meta(&acc[base], true, false),
        meta(&acc[base + 1], false, false),
        meta(&acc[base + 2], true, false),
        meta(&acc[base + 3], true, false),
        meta(user_in, true, false),
        meta(user_out, true, false),
        meta(&acc[base + 6], false, false),
        meta(&acc[base + 7], false, false),
        meta(&acc[base + 4], true, false),
        meta(&acc[base + 5], false, false),
        meta(&acc[ACC_AUTHORITY], true, true),
        meta(&acc[ACC_TOKEN_2022], false, false),
        meta(&acc[ACC_TOKEN_PROGRAM], false, false),
        meta(&acc[ACC_MEMO], false, false),
        meta(&acc[ACC_DLMM_EVENT], false, false),
        meta(&acc[ACC_DLMM], false, false),
        meta(&acc[base + 8], true, false),
        meta(&acc[base + 9], true, false),
    ];
    let infos = [
        &acc[base],
        &acc[base + 1],
        &acc[base + 2],
        &acc[base + 3],
        user_in,
        user_out,
        &acc[base + 6],
        &acc[base + 7],
        &acc[base + 4],
        &acc[base + 5],
        &acc[ACC_AUTHORITY],
        &acc[ACC_TOKEN_2022],
        &acc[ACC_TOKEN_PROGRAM],
        &acc[ACC_MEMO],
        &acc[ACC_DLMM_EVENT],
        &acc[ACC_DLMM],
        &acc[base + 8],
        &acc[base + 9],
    ];
    let ix = Instruction {
        program_id: acc[ACC_DLMM].key(),
        accounts: &metas,
        data: &data,
    };
    slice_invoke(&ix, &infos).map_err(|_| err(ERR_DLMM))
}

fn process(_pid: &Pubkey, accounts: &[AccountInfo], data: &[u8]) -> Result<(), ProgramError> {
    if data.len() != 26 || data[..8] != DISC {
        return Err(err(ERR_IX));
    }
    if accounts.len() < N_ACC {
        return Err(err(ERR_ACCOUNTS));
    }
    if !accounts[ACC_AUTHORITY].is_signer() {
        return Err(err(ERR_SIGNER));
    }
    let amount_in = load_u64(&data[8..16]);
    let min_profit = load_u64(&data[16..24]);
    let dir0 = data[24];
    let dir1 = data[25];
    if amount_in == 0 || dir0 > 1 || dir1 > 1 {
        return Err(err(ERR_IX));
    }

    let q0 = tok_amt(&accounts[ACC_USER_QUOTE])?;
    let b0 = tok_amt(&accounts[ACC_USER_BASE])?;

    cpi_dlmm_hop(accounts, HOP0, amount_in, dir0 == 1)?;
    let b1 = tok_amt(&accounts[ACC_USER_BASE])?;
    let mid = if dir0 == 1 {
        b1.saturating_sub(b0)
    } else {
        tok_amt(&accounts[ACC_USER_QUOTE])?.saturating_sub(q0)
    };
    if mid == 0 {
        return Err(err(ERR_DLMM));
    }
    cpi_dlmm_hop(accounts, HOP1, mid, dir1 == 1)?;

    let q1 = tok_amt(&accounts[ACC_USER_QUOTE])?;
    if q1 < q0 || q1 - q0 < min_profit {
        return Err(err(ERR_PROFIT));
    }
    Ok(())
}
