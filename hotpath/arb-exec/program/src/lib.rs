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

const DISC: [u8; 8] = *b"ARBEXEC0";
const TOKEN_2022_IDX: usize = 35;
const PUMP_REMAIN_OFF: usize = 36;
const PUMP_N_SELL: usize = 24;
const PUMP_N_BUY: usize = 26;

const ACC_AUTHORITY: usize = 0;
const ACC_USER_QUOTE: usize = 1;
const ACC_USER_BASE: usize = 2;
const ACC_TOKEN_PROGRAM: usize = 3;
const ACC_MEMO: usize = 6;
const ACC_DLMM: usize = 7;
const ACC_DLMM_EVENT: usize = 8;
const ACC_LB_PAIR: usize = 9;
const ACC_BITMAP: usize = 10;
const ACC_RESERVE_X: usize = 11;
const ACC_RESERVE_Y: usize = 12;
const ACC_ORACLE: usize = 13;
const ACC_HOST_FEE: usize = 14;
const ACC_MINT_X: usize = 15;
const ACC_MINT_Y: usize = 16;
const ACC_BIN0: usize = 17;
const ACC_BIN1: usize = 18;

const DIR_DLMM_THEN_PUMP: u8 = 0;

const DLMM_SWAP2: [u8; 8] = [0x41, 0x4b, 0x3f, 0x4c, 0xeb, 0x5b, 0x5b, 0x88];
const PUMP_SELL: [u8; 8] = [0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad];
const PUMP_BUY_EXACT_Q: [u8; 8] = [0xc6, 0x2e, 0x15, 0x52, 0xb4, 0xd9, 0xe8, 0x70];

const ERR_IX: u32 = 1;
const ERR_ACCOUNTS: u32 = 2;
const ERR_SIGNER: u32 = 3;
const ERR_DLMM: u32 = 4;
const ERR_PUMP: u32 = 5;
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

fn pump_writable_sell(i: usize) -> bool {
    matches!(i, 0 | 1 | 5 | 6 | 7 | 8 | 10 | 17 | 23)
}

fn pump_writable_buy(i: usize) -> bool {
    /* BuyExactQuoteIn: sell writables plus gvol@19, uvol@20, fee_ata@25. */
    matches!(i, 0 | 1 | 5 | 6 | 7 | 8 | 10 | 17 | 19 | 20 | 25)
}

#[inline(never)]
fn cpi_dlmm(acc: &[AccountInfo], amount_in: u64, token_in_is_quote: bool) -> Result<(), ProgramError> {
    let (user_in, user_out): (&AccountInfo, &AccountInfo) = if token_in_is_quote {
        (&acc[ACC_USER_QUOTE], &acc[ACC_USER_BASE])
    } else {
        (&acc[ACC_USER_BASE], &acc[ACC_USER_QUOTE])
    };
    let t22 = &acc[TOKEN_2022_IDX];
    let mut data = [0u8; 28];
    data[..8].copy_from_slice(&DLMM_SWAP2);
    data[8..16].copy_from_slice(&amount_in.to_le_bytes());

    let metas = [
        meta(&acc[ACC_LB_PAIR], true, false),
        meta(&acc[ACC_BITMAP], false, false),
        meta(&acc[ACC_RESERVE_X], true, false),
        meta(&acc[ACC_RESERVE_Y], true, false),
        meta(user_in, true, false),
        meta(user_out, true, false),
        meta(&acc[ACC_MINT_X], false, false),
        meta(&acc[ACC_MINT_Y], false, false),
        meta(&acc[ACC_ORACLE], true, false),
        meta(&acc[ACC_HOST_FEE], false, false),
        meta(&acc[ACC_AUTHORITY], true, true),
        meta(t22, false, false),
        meta(&acc[ACC_TOKEN_PROGRAM], false, false),
        meta(&acc[ACC_MEMO], false, false),
        meta(&acc[ACC_DLMM_EVENT], false, false),
        meta(&acc[ACC_DLMM], false, false),
        meta(&acc[ACC_BIN0], true, false),
        meta(&acc[ACC_BIN1], true, false),
    ];
    let infos = [
        &acc[ACC_LB_PAIR],
        &acc[ACC_BITMAP],
        &acc[ACC_RESERVE_X],
        &acc[ACC_RESERVE_Y],
        user_in,
        user_out,
        &acc[ACC_MINT_X],
        &acc[ACC_MINT_Y],
        &acc[ACC_ORACLE],
        &acc[ACC_HOST_FEE],
        &acc[ACC_AUTHORITY],
        t22,
        &acc[ACC_TOKEN_PROGRAM],
        &acc[ACC_MEMO],
        &acc[ACC_DLMM_EVENT],
        &acc[ACC_DLMM],
        &acc[ACC_BIN0],
        &acc[ACC_BIN1],
    ];
    let ix = Instruction {
        program_id: acc[ACC_DLMM].key(),
        accounts: &metas,
        data: &data,
    };
    slice_invoke(&ix, &infos).map_err(|_| err(ERR_DLMM))
}

#[inline(never)]
fn cpi_pump_sell(acc: &[AccountInfo], amount_in: u64) -> Result<(), ProgramError> {
    if acc.len() < PUMP_REMAIN_OFF + PUMP_N_SELL {
        return Err(err(ERR_ACCOUNTS));
    }
    let pump = &acc[PUMP_REMAIN_OFF..PUMP_REMAIN_OFF + PUMP_N_SELL];
    let mut data = [0u8; 24];
    data[..8].copy_from_slice(&PUMP_SELL);
    data[8..16].copy_from_slice(&amount_in.to_le_bytes());
    let auth = acc[ACC_AUTHORITY].key();
    let metas: [AccountMeta; PUMP_N_SELL] = core::array::from_fn(|i| {
        let a = &pump[i];
        meta(a, pump_writable_sell(i), a.key() == auth)
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

#[inline(never)]
fn cpi_pump_buy(acc: &[AccountInfo], amount_in: u64) -> Result<(), ProgramError> {
    if acc.len() < PUMP_REMAIN_OFF + PUMP_N_BUY {
        return Err(err(ERR_ACCOUNTS));
    }
    let pump = &acc[PUMP_REMAIN_OFF..PUMP_REMAIN_OFF + PUMP_N_BUY];
    /* Live BuyExactQuoteIn is 24 B: spendable_quote_in + min_base_amount_out=1. */
    let mut data = [0u8; 24];
    data[..8].copy_from_slice(&PUMP_BUY_EXACT_Q);
    data[8..16].copy_from_slice(&amount_in.to_le_bytes());
    data[16..24].copy_from_slice(&1u64.to_le_bytes());
    let auth = acc[ACC_AUTHORITY].key();
    let metas: [AccountMeta; PUMP_N_BUY] = core::array::from_fn(|i| {
        let a = &pump[i];
        meta(a, pump_writable_buy(i), a.key() == auth)
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
    if data.len() != 25 || data[..8] != DISC {
        return Err(err(ERR_IX));
    }
    if accounts.len() < PUMP_REMAIN_OFF + PUMP_N_SELL {
        return Err(err(ERR_ACCOUNTS));
    }
    if !accounts[ACC_AUTHORITY].is_signer() {
        return Err(err(ERR_SIGNER));
    }
    let direction = data[8];
    let amount_in = load_u64(&data[9..17]);
    let min_profit = load_u64(&data[17..25]);
    if direction > 1 || amount_in == 0 {
        return Err(err(ERR_IX));
    }

    let q0 = tok_amt(&accounts[ACC_USER_QUOTE])?;
    let b0 = tok_amt(&accounts[ACC_USER_BASE])?;

    if direction == DIR_DLMM_THEN_PUMP {
        cpi_dlmm(accounts, amount_in, true)?;
        let b1 = tok_amt(&accounts[ACC_USER_BASE])?;
        let mid = b1.saturating_sub(b0);
        if mid == 0 {
            return Err(err(ERR_DLMM));
        }
        cpi_pump_sell(accounts, mid)?;
    } else {
        if accounts.len() < PUMP_REMAIN_OFF + PUMP_N_BUY {
            return Err(err(ERR_ACCOUNTS));
        }
        cpi_pump_buy(accounts, amount_in)?;
        let b1 = tok_amt(&accounts[ACC_USER_BASE])?;
        let mid = b1.saturating_sub(b0);
        if mid == 0 {
            return Err(err(ERR_PUMP));
        }
        cpi_dlmm(accounts, mid, false)?;
    }

    let q1 = tok_amt(&accounts[ACC_USER_QUOTE])?;
    if q1 < q0 || q1 - q0 < min_profit {
        return Err(err(ERR_PROFIT));
    }
    Ok(())
}
