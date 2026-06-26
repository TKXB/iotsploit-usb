//! IEEE 488.2 definite-length arbitrary block support.
//!
//! Wire format produced by libscpi (`SCPI_ResultArbitraryBlock`):
//!
//! ```text
//! #<ndigits><length><payload>
//! ```
//!
//! - `#`         start marker
//! - `ndigits`   one ASCII digit `1`..`9`, the number of digits that follow
//! - `length`    `ndigits` ASCII digits giving the payload byte count
//! - `payload`   exactly `length` raw bytes (may contain `0x00`, newline, `#`, ...)
//!
//! libscpi appends a single `\n` message terminator after every command handler
//! returns, so a block response on the wire is typically `#..payload\n`. The
//! parser ignores any trailing bytes after the declared payload.

use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BlockError {
    /// Input is empty.
    Empty,
    /// First byte is not `#`.
    NoHash,
    /// The digit-count byte is not `1`..`9`.
    BadDigitCount,
    /// End of input reached before the length field / payload could be read.
    UnexpectedEof,
    /// A byte inside the length field was not an ASCII digit.
    InvalidLength,
    /// Declared payload length exceeds the configured maximum.
    TooLarge { len: usize, max: usize },
    /// Declared payload length is larger than the bytes actually present.
    PayloadTooShort,
}

impl fmt::Display for BlockError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            BlockError::Empty => write!(f, "empty block input"),
            BlockError::NoHash => write!(f, "block does not start with '#'"),
            BlockError::BadDigitCount => write!(f, "invalid digit-count byte (must be 1..9)"),
            BlockError::UnexpectedEof => write!(f, "unexpected end of input in block header"),
            BlockError::InvalidLength => write!(f, "non-digit byte in block length field"),
            BlockError::TooLarge { len, max } => {
                write!(f, "declared block length {} exceeds maximum {}", len, max)
            }
            BlockError::PayloadTooShort => {
                write!(f, "declared payload longer than available bytes")
            }
        }
    }
}

impl std::error::Error for BlockError {}

/// Parse an IEEE 488.2 definite-length arbitrary block and return the payload
/// slice (a view into `msg`).
///
/// Trailing bytes after the payload (e.g. the SCPI `\n` message terminator) are
/// ignored. `max_len` caps the accepted declared length; pass [`usize::MAX`] to
/// disable the cap.
pub fn parse_block(msg: &[u8], max_len: usize) -> Result<&[u8], BlockError> {
    if msg.is_empty() {
        return Err(BlockError::Empty);
    }
    if msg[0] != b'#' {
        return Err(BlockError::NoHash);
    }
    let mut i = 1;
    if i >= msg.len() {
        return Err(BlockError::UnexpectedEof);
    }
    let ndigits = match msg[i] {
        c @ b'1'..=b'9' => (c - b'0') as usize,
        _ => return Err(BlockError::BadDigitCount),
    };
    i += 1;
    if i + ndigits > msg.len() {
        return Err(BlockError::UnexpectedEof);
    }
    let len_field = &msg[i..i + ndigits];
    let mut len: usize = 0;
    let mut overflow = false;
    for &c in len_field {
        match c {
            b'0'..=b'9' => {
                match len
                    .checked_mul(10)
                    .and_then(|v| v.checked_add((c - b'0') as usize))
                {
                    Some(v) => len = v,
                    None => overflow = true,
                }
            }
            _ => return Err(BlockError::InvalidLength),
        }
    }
    if overflow {
        return Err(BlockError::TooLarge { len: usize::MAX, max: max_len });
    }
    i += ndigits;
    if len > max_len {
        return Err(BlockError::TooLarge { len, max: max_len });
    }
    if i + len > msg.len() {
        return Err(BlockError::PayloadTooShort);
    }
    Ok(&msg[i..i + len])
}

/// Encode `payload` as `#<ndigits><length><payload>`.
///
/// Mirrors `SCPI_ResultArbitraryBlockHeader` from libscpi.
pub fn encode_block(payload: &[u8]) -> Vec<u8> {
    let len = payload.len();
    let len_str = len.to_string();
    let ndigits = len_str.len();
    debug_assert!((1..=9).contains(&ndigits), "payload length overflows 9 digits");
    let mut out = Vec::with_capacity(2 + ndigits + len);
    out.push(b'#');
    out.push(b'0' + ndigits as u8);
    out.extend_from_slice(len_str.as_bytes());
    out.extend_from_slice(payload);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_empty_block() {
        // libscpi for a 0-length payload: "#10" + trailing terminator.
        let msg = b"#10\n";
        let p = parse_block(msg, usize::MAX).unwrap();
        assert!(p.is_empty());
    }

    #[test]
    fn parse_simple_block() {
        // 4-byte payload "1234": '#' + ndigits '1' + length digit '4' + payload
        let msg = b"#141234";
        let p = parse_block(msg, usize::MAX).unwrap();
        assert_eq!(p, b"1234");
    }

    #[test]
    fn parse_block_with_trailing_newline() {
        let mut msg = encode_block(b"hello");
        msg.push(b'\n');
        let p = parse_block(&msg, usize::MAX).unwrap();
        assert_eq!(p, b"hello");
    }

    #[test]
    fn parse_preserves_arbitrary_bytes() {
        // 0x00, newline, CR, comma, quote, '#'
        let payload = [0x00, b'\n', b'\r', b',', b'"', b'#'];
        let msg = encode_block(&payload);
        let p = parse_block(&msg, usize::MAX).unwrap();
        assert_eq!(p, &payload[..]);
    }

    #[test]
    fn parse_rejects_missing_hash() {
        assert_eq!(parse_block(b"10", usize::MAX), Err(BlockError::NoHash));
    }

    #[test]
    fn parse_rejects_empty() {
        assert_eq!(parse_block(b"", usize::MAX), Err(BlockError::Empty));
    }

    #[test]
    fn parse_rejects_bad_digit_count() {
        assert_eq!(parse_block(b"#0", usize::MAX), Err(BlockError::BadDigitCount));
        assert_eq!(parse_block(b"#a12", usize::MAX), Err(BlockError::BadDigitCount));
    }

    #[test]
    fn parse_rejects_non_digit_in_length() {
        assert_eq!(parse_block(b"#2x4", usize::MAX), Err(BlockError::InvalidLength));
    }

    #[test]
    fn parse_rejects_unexpected_eof() {
        assert_eq!(parse_block(b"#", usize::MAX), Err(BlockError::UnexpectedEof));
        assert_eq!(parse_block(b"#3", usize::MAX), Err(BlockError::UnexpectedEof));
        assert_eq!(parse_block(b"#31", usize::MAX), Err(BlockError::UnexpectedEof));
    }

    #[test]
    fn parse_rejects_payload_too_short() {
        // declares 5 bytes, only 3 present
        assert_eq!(parse_block(b"#15abc", usize::MAX), Err(BlockError::PayloadTooShort));
    }

    #[test]
    fn parse_rejects_too_large() {
        let msg = b"#49999";
        assert_eq!(
            parse_block(msg, 100),
            Err(BlockError::TooLarge { len: 9999, max: 100 })
        );
    }

    #[test]
    fn encode_roundtrips() {
        for payload in [b"".as_ref(), b"a", b"hello", &[0u8, 1, 2, 3][..]] {
            let msg = encode_block(payload);
            let p = parse_block(&msg, usize::MAX).unwrap();
            assert_eq!(p, payload);
        }
    }

    #[test]
    fn encode_matches_libscpi_format() {
        assert_eq!(encode_block(b""), b"#10");
        assert_eq!(encode_block(b"1234"), b"#141234");
        let m = encode_block(&[0u8; 16]);
        assert_eq!(&m[..4], b"#216");
        assert_eq!(m.len(), 4 + 16);
    }
}
