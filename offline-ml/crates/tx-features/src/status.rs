/// Encoder status. `Ok` is the only success value.
///
/// On any other status the output header is cleared and the remaining output
/// bytes are undefined. Callers must not read features unless the status is `Ok`.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    Ok = 0,
    Malformed = 1,
    UnsupportedVersion = 2,
    CapacityExceeded = 3,
    /// FFI only: a required pointer was null.
    NullArgument = 4,
    /// FFI only: a panic was caught at the ABI boundary.
    Internal = 5,
}

impl Status {
    #[inline]
    pub const fn as_i32(self) -> i32 {
        self as i32
    }

    pub const fn name(self) -> &'static str {
        match self {
            Status::Ok => "OK",
            Status::Malformed => "MALFORMED",
            Status::UnsupportedVersion => "UNSUPPORTED_VERSION",
            Status::CapacityExceeded => "CAPACITY_EXCEEDED",
            Status::NullArgument => "NULL_ARGUMENT",
            Status::Internal => "INTERNAL",
        }
    }
}
