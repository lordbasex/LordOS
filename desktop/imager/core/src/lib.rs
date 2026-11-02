//! LordOS Imager core: what the desktop app (`macos`, `windows`, `linux`) does below its UI. The rules are
//! the native imager's (server/src/imager/, docs/IMAGER.md), written again in Rust and checked
//! against the same test vectors:
//!
//! - [`sums`]: `SHA256SUMS.sig` is base64 of the Ed25519 signature over the exact bytes of
//!   `SHA256SUMS`, made with the LordOS release key ([`RELEASE_KEY`], fingerprint `c7155b8e…`);
//! - [`xzinfo`]: the uncompressed size of an `.img.xz`, from its index, before anything is erased;
//! - [`write`]: image → device, hashing what goes out, reading it back to compare, wiping the
//!   ends of the device when it fails or is cancelled;
//! - [`drives`]: only removable, non-system drives, per platform;
//! - [`secrets`]: the password's SHA-512 crypt hash and the Wi-Fi network's derived WPA2 key.
pub mod drives;
pub mod secrets;
pub mod sums;
pub mod write;
pub mod xzinfo;

/// The LordOS release key (tools/release/lordos-release.pub.pem), raw 32 bytes. Its SHA-256
/// fingerprint over the DER form is c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e
/// (checked in tests).
pub const RELEASE_KEY: [u8; 32] = [
    0xf1, 0xca, 0xb0, 0x0f, 0x31, 0xfe, 0x65, 0x53, 0x1d, 0x06, 0xa3, 0x63, 0x0b, 0x7d, 0x78, 0xd5,
    0xd9, 0xa8, 0xde, 0x07, 0x07, 0xf0, 0xf5, 0xad, 0xf3, 0xc2, 0x3e, 0x44, 0x4f, 0x5d, 0x39, 0xa0,
];

/// Errors are stable codes (the UI translates them with the same keys as the native app:
/// errors.imager.* and errors.disk.*), plus a detail for logs.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct Error {
    pub code: &'static str,
    pub detail: String,
}

impl Error {
    pub fn new(code: &'static str, detail: impl Into<String>) -> Self {
        Error { code, detail: detail.into() }
    }
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.detail.is_empty() { write!(f, "{}", self.code) } else { write!(f, "{}: {}", self.code, self.detail) }
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

/// A file name the imager accepts: [A-Za-z0-9._+~-], 1..128, not starting with '.' or '-'.
pub fn valid_name(n: &str) -> bool {
    !n.is_empty() && n.len() <= 128 && !n.starts_with('.') && !n.starts_with('-') && !n.contains("..")
        && n.bytes().all(|c| c.is_ascii_alphanumeric() || b"._+~-".contains(&c))
}

/// 1 raw (.img, .iso), 2 xz (.img.xz), 0 anything else.
pub fn image_kind(n: &str) -> u8 {
    if n.len() > 7 && n.ends_with(".img.xz") { 2 } else if (n.len() > 4 && n.ends_with(".img")) || (n.len() > 4 && n.ends_with(".iso")) { 1 } else { 0 }
}

/// v<1-4 digits>.<…>.<…>[-<[a-z0-9.]+>], up to 32 characters.
pub fn valid_tag(t: &str) -> bool {
    if t.len() < 6 || t.len() > 32 || !t.starts_with('v') { return false; }
    let (ver, pre) = match t[1..].split_once('-') { Some((v, p)) => (v, Some(p)), None => (&t[1..], None) };
    let parts: Vec<&str> = ver.split('.').collect();
    // three numbers, or four for a point release (v0.8.4.1-beta)
    if !(parts.len() == 3 || parts.len() == 4) || parts.iter().any(|p| p.is_empty() || p.len() > 4 || !p.bytes().all(|c| c.is_ascii_digit())) { return false; }
    match pre { None => true, Some(p) => !p.is_empty() && p.bytes().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'.') }
}
