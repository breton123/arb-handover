//! C ABI. Panics are caught here and never cross the boundary.
//!
//! The live searcher should prefer [`crate::encode_transaction_v1`]. This wrapper
//! exists for non-Rust framers. It zeroes `scratch` on entry so a C caller can
//! pass uninitialized scratch memory.

use crate::features::{encode_transaction_v1, encode_transaction_v2};
use crate::limits::FEATURE_SCHEMA_ID;
use crate::status::Status;
use crate::view::{EncodedTxV1, EncodedTxV2, Scratch};
use std::panic::{catch_unwind, AssertUnwindSafe};

/// Bytes required for [`Scratch`].
#[no_mangle]
pub extern "C" fn txf_scratch_bytes() -> usize {
    core::mem::size_of::<Scratch>()
}

/// Bytes required for [`EncodedTxV1`].
#[no_mangle]
pub extern "C" fn txf_encoded_v1_bytes() -> usize {
    core::mem::size_of::<EncodedTxV1>()
}

/// Bytes required for [`EncodedTxV2`].
#[no_mangle]
pub extern "C" fn txf_encoded_v2_bytes() -> usize {
    core::mem::size_of::<EncodedTxV2>()
}

#[no_mangle]
pub extern "C" fn txf_feature_schema_id() -> u32 {
    FEATURE_SCHEMA_ID
}

/// Zero `scratch`. Optional: [`txf_encode_v1`] already zeroes it.
///
/// # Safety
/// `scratch` must be non-null and point at [`txf_scratch_bytes`] writable bytes.
#[no_mangle]
pub unsafe extern "C" fn txf_scratch_init(scratch: *mut Scratch) {
    if scratch.is_null() {
        return;
    }
    unsafe {
        core::ptr::write_bytes(scratch, 0, 1);
    }
}

/// Encode one framed transaction.
///
/// # Safety
/// - `raw` may be null only when `raw_len` is 0.
/// - `scratch` and `out` must be non-null and writable for their full sizes.
/// - `raw` must remain valid for the call.
///
/// Returns a [`Status`] code. On failure `out.header` is zero and other `out`
/// bytes are undefined.
#[no_mangle]
pub unsafe extern "C" fn txf_encode_v1(
    raw: *const u8,
    raw_len: usize,
    scratch: *mut Scratch,
    out: *mut EncodedTxV1,
) -> i32 {
    if scratch.is_null() || out.is_null() {
        return Status::NullArgument.as_i32();
    }
    if raw.is_null() && raw_len != 0 {
        return Status::NullArgument.as_i32();
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        unsafe {
            core::ptr::write_bytes(scratch, 0, 1);
            let bytes = if raw_len == 0 {
                &[]
            } else {
                core::slice::from_raw_parts(raw, raw_len)
            };
            encode_transaction_v1(bytes, &mut *scratch, &mut *out).as_i32()
        }
    }));
    match result {
        Ok(code) => code,
        Err(_) => Status::Internal.as_i32(),
    }
}

/// framed-tx-v2: [`txf_encode_v1`] plus the v1 transaction format.
///
/// # Safety
/// Same contract as [`txf_encode_v1`], with `out` sized [`txf_encoded_v2_bytes`].
/// On failure `out.base.header` and `out.ext` are zero.
#[no_mangle]
pub unsafe extern "C" fn txf_encode_v2(
    raw: *const u8,
    raw_len: usize,
    scratch: *mut Scratch,
    out: *mut EncodedTxV2,
) -> i32 {
    if scratch.is_null() || out.is_null() {
        return Status::NullArgument.as_i32();
    }
    if raw.is_null() && raw_len != 0 {
        return Status::NullArgument.as_i32();
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        unsafe {
            core::ptr::write_bytes(scratch, 0, 1);
            let bytes = if raw_len == 0 {
                &[]
            } else {
                core::slice::from_raw_parts(raw, raw_len)
            };
            encode_transaction_v2(bytes, &mut *scratch, &mut *out).as_i32()
        }
    }));
    match result {
        Ok(code) => code,
        Err(_) => Status::Internal.as_i32(),
    }
}
