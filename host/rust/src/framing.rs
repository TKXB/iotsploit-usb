//! The envelope a device puts on its USB vendor pipe once the host sends
//! `SYSTem:STReam:FRAMing 1`: `[u8 type][u8 0][u16 len LE]` then `len` bytes.
//! Type 0x01 is log text, 0x02 one data-plane record. Mirrors
//! `examples/esp32s3/main/usb_frame.h` and `examples/stm32f4disco/can_stream.h`.
//!
//! Framing is off at boot so a host that only reads log text is not garbled,
//! which means a reader can attach mid-stream or find unframed text queued
//! ahead of the first frame. The decoder therefore resynchronises: a byte that
//! cannot begin a plausible frame is skipped.

/// Envelope header length.
pub const FRAME_HDR: usize = 4;
/// Log text.
pub const FRAME_LOG: u8 = 0x01;
/// One record.
pub const FRAME_REC: u8 = 0x02;
/// Largest payload a frame may carry; anything longer is taken as noise.
pub const FRAME_MAX: usize = 512;

/// A decoded frame.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Frame {
    Log(Vec<u8>),
    Rec(Vec<u8>),
}

/// Incremental decoder: push bytes as they arrive, take whole frames out.
#[derive(Debug, Default)]
pub struct FrameDecoder {
    buf: Vec<u8>,
    skipped: u64,
}

impl FrameDecoder {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn push(&mut self, bytes: &[u8]) {
        self.buf.extend_from_slice(bytes);
    }

    /// Bytes discarded while resynchronising, since creation.
    pub fn skipped(&self) -> u64 {
        self.skipped
    }

    /// The next whole frame, or `None` until more bytes arrive.
    pub fn next_frame(&mut self) -> Option<Frame> {
        let mut i = 0usize;
        let frame = loop {
            if i + FRAME_HDR > self.buf.len() {
                break None;
            }
            let (t, rsv) = (self.buf[i], self.buf[i + 1]);
            let len = u16::from_le_bytes([self.buf[i + 2], self.buf[i + 3]]) as usize;
            if !((t == FRAME_LOG || t == FRAME_REC) && rsv == 0 && len > 0 && len <= FRAME_MAX) {
                i += 1;
                continue;
            }
            if i + FRAME_HDR + len > self.buf.len() {
                break None;
            }
            let payload = self.buf[i + FRAME_HDR..i + FRAME_HDR + len].to_vec();
            self.skipped += i as u64;
            self.buf.drain(..i + FRAME_HDR + len);
            i = 0;
            break Some(if t == FRAME_REC { Frame::Rec(payload) } else { Frame::Log(payload) });
        };
        if i > 0 {
            self.skipped += i as u64;
            self.buf.drain(..i);
        }
        frame
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(t: u8, payload: &[u8]) -> Vec<u8> {
        let mut v = vec![t, 0];
        v.extend_from_slice(&(payload.len() as u16).to_le_bytes());
        v.extend_from_slice(payload);
        v
    }

    #[test]
    fn decodes_frames_split_across_reads() {
        let mut bytes = frame(FRAME_REC, &[1u8; 32]);
        bytes.extend(frame(FRAME_LOG, b"hello\n"));
        bytes.extend(frame(FRAME_REC, &[2u8; 32]));
        let mut d = FrameDecoder::new();
        let mut out = Vec::new();
        // 64-byte USB packets do not line up with 36-byte record frames.
        for chunk in bytes.chunks(64) {
            d.push(chunk);
            while let Some(f) = d.next_frame() {
                out.push(f);
            }
        }
        assert_eq!(
            out,
            [Frame::Rec(vec![1; 32]), Frame::Log(b"hello\n".to_vec()), Frame::Rec(vec![2; 32])]
        );
        assert_eq!(d.skipped(), 0);
    }

    #[test]
    fn resynchronises_past_unframed_text() {
        let mut bytes = b"I (12) boot: unframed\n".to_vec();
        bytes.extend(frame(FRAME_REC, &[7u8; 8]));
        let mut d = FrameDecoder::new();
        d.push(&bytes);
        assert_eq!(d.next_frame(), Some(Frame::Rec(vec![7; 8])));
        assert_eq!(d.next_frame(), None);
        assert_eq!(d.skipped(), 22);
    }

    #[test]
    fn waits_for_a_partial_frame() {
        let bytes = frame(FRAME_REC, &[3u8; 32]);
        let mut d = FrameDecoder::new();
        d.push(&bytes[..20]);
        assert_eq!(d.next_frame(), None);
        d.push(&bytes[20..]);
        assert_eq!(d.next_frame(), Some(Frame::Rec(vec![3; 32])));
    }
}
