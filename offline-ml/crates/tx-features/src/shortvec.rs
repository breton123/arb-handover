//! Solana compact-u16 (shortvec) decoder.
//!
//! Same rules as the runtime decoder: at most 3 bytes, continuation on every
//! byte except the last, value must fit in `u16`. Overlong encodings are
//! accepted because the chain accepts them. This module does not encode.

use crate::status::Status;

/// Decode one compact-u16 starting at `index`.
///
/// Returns `(value, index_after)`.
pub(crate) fn decode_short_u16(raw: &[u8], index: usize) -> Result<(usize, usize), Status> {
    let mut value: usize = 0;
    let mut size = 0usize;
    loop {
        if index.checked_add(size).map(|p| p >= raw.len()).unwrap_or(true) {
            return Err(Status::Malformed);
        }
        let elem = raw[index + size];
        let bits = (elem & 0x7f) as usize;
        value |= bits << (size * 7);
        size += 1;
        if elem & 0x80 == 0 {
            break;
        }
        if size >= 3 {
            return Err(Status::Malformed);
        }
    }
    if value > u16::MAX as usize {
        return Err(Status::Malformed);
    }
    let next = index + size;
    Ok((value, next))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_known_values() {
        assert_eq!(decode_short_u16(&[0x00], 0).unwrap(), (0, 1));
        assert_eq!(decode_short_u16(&[0x7f], 0).unwrap(), (127, 1));
        assert_eq!(decode_short_u16(&[0x80, 0x01], 0).unwrap(), (128, 2));
        // overlong zero, accepted
        assert_eq!(decode_short_u16(&[0x80, 0x00], 0).unwrap(), (0, 2));
        assert_eq!(decode_short_u16(&[0xff, 0xff, 0x03], 0).unwrap(), (65535, 3));
    }

    #[test]
    fn rejects_truncated_and_overflow() {
        assert_eq!(decode_short_u16(&[], 0), Err(Status::Malformed));
        assert_eq!(decode_short_u16(&[0x80], 0), Err(Status::Malformed));
        assert_eq!(decode_short_u16(&[0x80, 0x80], 0), Err(Status::Malformed));
        assert_eq!(
            decode_short_u16(&[0x80, 0x80, 0x80], 0),
            Err(Status::Malformed)
        );
        // 65536 = continuation, continuation, 0x04
        assert_eq!(
            decode_short_u16(&[0x80, 0x80, 0x04], 0),
            Err(Status::Malformed)
        );
    }
}
