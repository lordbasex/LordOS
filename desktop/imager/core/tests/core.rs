//! The desktop imager's core against the same fixtures and vectors as the native one
//! (server/tests/imager_test.c, firstrun_test.c): release signature good / tampered / missing /
//! another release's, the key fingerprint, the .xz index, the secrets, and write + verify + cancel
//! on a file standing in for a device.
use lordos_imager_core::{sums, write, xzinfo, secrets, RELEASE_KEY, image_kind, valid_name, valid_tag};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{Read, Write};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};

fn fixture(name: &str) -> PathBuf { PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../server/tests/imager").join(name) }

#[test]
fn release_key_matches_the_published_fingerprint() {
    let pem = fs::read_to_string(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../tools/release/lordos-release.pub.pem")).unwrap();
    let b64: String = pem.lines().filter(|l| !l.starts_with("-----")).collect();
    use base64::Engine;
    let der = base64::engine::general_purpose::STANDARD.decode(b64).unwrap();
    assert_eq!(&der[der.len() - 32..], &RELEASE_KEY);
    assert_eq!(sums::hex(&Sha256::digest(&der)), "c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e");
}

#[test]
fn signature_good_bad_missing() {
    let s = fs::read(fixture("release-SHA256SUMS")).unwrap();
    let sig = fs::read(fixture("release-SHA256SUMS.sig")).unwrap();
    assert!(sums::verify(&s, &sig, &RELEASE_KEY).is_ok());
    assert!(sums::verify(&s, sig.strip_suffix(b"\n").unwrap(), &RELEASE_KEY).is_ok());
    let mut t = s.clone();
    t[10] ^= 1;
    assert_eq!(sums::verify(&t, &sig, &RELEASE_KEY).unwrap_err().code, "imager.badSignature");
    assert!(sums::verify(&s[..s.len() - 1], &sig, &RELEASE_KEY).is_err());
    assert!(sums::verify(&s, b"", &RELEASE_KEY).is_err());
    assert!(sums::verify(&s, b"M2obKQ", &RELEASE_KEY).is_err());
    // a real signature of another release (0.8.0) over this list
    assert!(sums::verify(&s, b"NzFKWqpHNft14vKPCOK6q0DaQNef9jic0/7TUa7wkocU0g2AnRb7Gg2MBWljOsKwUK/0ceCAlkuBpRmjtSqPAw==\n", &RELEASE_KEY).is_err());
    let mut other = RELEASE_KEY;
    other[0] ^= 0x40;
    assert!(sums::verify(&s, &sig, &other).is_err());
    let list = sums::parse(&s).unwrap();
    assert_eq!(sums::hex(&list["lordos-0.8.1-rpi5-arm64.img.xz"]), "a7714f4f3d437a9e48088cb89579a93638b36e8a93944add23e18a126f04ea45");
    assert!(sums::parse(b"zz").is_err());
    let dup = b"a7714f4f3d437a9e48088cb89579a93638b36e8a93944add23e18a126f04ea45  x.img\na7714f4f3d437a9e48088cb89579a93638b36e8a93944add23e18a126f04ea45  x.img\n";
    assert!(sums::parse(dup).is_err());
}

#[test]
fn names_and_tags() {
    assert!(valid_name("lordos-0.8.4-rpi5-arm64.img.xz") && !valid_name("../x.img") && !valid_name(".x") && !valid_name("a b.img"));
    assert_eq!(image_kind("a.img.xz"), 2);
    assert_eq!(image_kind("a.iso"), 1);
    assert_eq!(image_kind("a.zip"), 0);
    assert!(valid_tag("v0.8.4-beta") && valid_tag("v1.0.0") && !valid_tag("v0.8") && !valid_tag("v0.8.4-") && !valid_tag("v0.8.4/../x"));
    assert!(valid_tag("v0.8.4.1-beta") && valid_tag("v0.8.4.1") && !valid_tag("v0.8.4.1.2") && !valid_tag("v0.8.4.") && !valid_tag("v0.8.4.-beta"));
}

#[test]
fn xz_index_sizes() {
    for (f, n) in [("one.xz", 23000), ("blocks.xz", 23000), ("two.xz", 24000), ("padded.xz", 23000)] {
        let mut file = fs::File::open(fixture(f)).unwrap();
        let len = file.metadata().unwrap().len();
        assert_eq!(xzinfo::uncompressed_size(&mut file, len), Some(n), "{f}");
    }
    let mut zeros = std::io::Cursor::new(vec![0u8; 64]);
    assert_eq!(xzinfo::uncompressed_size(&mut zeros, 64), None);
    assert_eq!(xzinfo::crc32(b"123456789"), 0xcbf4_3926);
    // every flipped byte in the last 64 never gives a different valid size
    let data = fs::read(fixture("one.xz")).unwrap();
    for k in data.len() - 64..data.len() {
        let mut d = data.clone();
        d[k] ^= 0x55;
        let len = d.len() as u64;
        let r = xzinfo::uncompressed_size(&mut std::io::Cursor::new(d), len);
        assert!(r.is_none() || r == Some(23000));
    }
}

#[test]
fn secrets_vectors() {
    // IEEE 802.11i, H.4.1
    assert_eq!(secrets::wifi_psk("IEEE", "password").unwrap(), "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
    assert!(secrets::wifi_psk("x", "short").is_err());
    assert!(secrets::wifi_psk(&"x".repeat(33), "password").is_err());
    // glibc's SHA-512 crypt vectors
    assert!(secrets::check_password("Hello world!", "$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1"));
    assert!(secrets::check_password("Hello world!", "$6$rounds=10000$saltstringsaltst$OW1/O6BYHV6BcXZu8QVeXbDWra3Oeqh0sbHbbMCVNSnCM/UrjmM0Dp8vOuZeHBy/YTBmSK6H9qs/y3RnOaw5v."));
    let h = secrets::hash_password("Correct-Horse-9-battery").unwrap();
    assert!(h.starts_with("$6$rounds=200000$"), "{h}");
    assert!(secrets::check_password("Correct-Horse-9-battery", &h) && !secrets::check_password("wrong", &h));
}

fn raw_image(dir: &std::path::Path, n: usize) -> (PathBuf, Vec<u8>) {
    let mut data = vec![0u8; n];
    let mut x = 0x1234_5678u32;
    for b in data.iter_mut().take(n / 2) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; *b = x as u8; }
    let p = dir.join("test.img");
    fs::write(&p, &data).unwrap();
    (p, data)
}

fn device(dir: &std::path::Path, size: u64) -> fs::File {
    let f = fs::OpenOptions::new().read(true).write(true).create(true).truncate(true).open(dir.join("device.bin")).unwrap();
    f.set_len(size).unwrap();
    f
}

#[test]
fn write_and_verify_raw_and_xz() {
    let dir = tempfile::tempdir().unwrap();
    let (img, data) = raw_image(dir.path(), 9 * 1024 * 1024 + 300);      // not a multiple of the sector size
    let cancel = AtomicBool::new(false);
    let i = write::inspect(&img).unwrap();
    let mut dev = device(dir.path(), 32 << 20);
    let listed: [u8; 32] = Sha256::digest(&data).into();
    let mut phases = vec![];
    let out = write::write_image(&i, &mut dev, 32 << 20, Some(listed), &cancel, |p| phases.push(p.phase)).unwrap();
    assert_eq!(out.sha256, sums::hex(&listed));
    assert!(phases.contains(&write::Phase::Write) && phases.contains(&write::Phase::Verify));
    let mut back = vec![0u8; data.len()];
    let mut f = fs::File::open(dir.path().join("device.bin")).unwrap();
    f.read_exact(&mut back).unwrap();
    assert_eq!(back, data);
    // the same through xz (two streams), with the file's own hash as the listed one
    let xzp = dir.path().join("test.img.xz");
    {
        let mut enc = xz2::write::XzEncoder::new(fs::File::create(&xzp).unwrap(), 6);
        enc.write_all(&data[..4 << 20]).unwrap();
        enc.finish().unwrap();
        let mut enc2 = xz2::write::XzEncoder::new(fs::OpenOptions::new().append(true).open(&xzp).unwrap(), 6);
        enc2.write_all(&data[4 << 20..]).unwrap();
        enc2.finish().unwrap();
    }
    let xi = write::inspect(&xzp).unwrap();
    assert!(xi.xz && xi.raw_size == data.len() as u64);
    let fh: [u8; 32] = Sha256::digest(fs::read(&xzp).unwrap()).into();
    let mut dev2 = device(dir.path(), 32 << 20);
    let out2 = write::write_image(&xi, &mut dev2, 32 << 20, Some(fh), &cancel, |_| {}).unwrap();
    assert_eq!(out2.sha256, sums::hex(&listed));
}

#[test]
fn refusals_and_cancel_wipe() {
    let dir = tempfile::tempdir().unwrap();
    let (img, data) = raw_image(dir.path(), 12 << 20);
    let i = write::inspect(&img).unwrap();
    let cancel = AtomicBool::new(false);
    // too large for the device
    let mut small = device(dir.path(), 8 << 20);
    assert_eq!(write::write_image(&i, &mut small, 8 << 20, None, &cancel, |_| {}).unwrap_err().code, "disk.imageTooLarge");
    // the file is not the one that was checked: the device is wiped
    let mut dev = device(dir.path(), 32 << 20);
    let wrong = [7u8; 32];
    assert_eq!(write::write_image(&i, &mut dev, 32 << 20, Some(wrong), &cancel, |_| {}).unwrap_err().code, "imager.imageChanged");
    let mut head = vec![0u8; 1 << 20];
    fs::File::open(dir.path().join("device.bin")).unwrap().read_exact(&mut head).unwrap();
    assert!(head.iter().all(|&b| b == 0), "the start of the device is wiped");
    // cancel in the middle of writing: wiped too
    let mut dev = device(dir.path(), 32 << 20);
    let c = AtomicBool::new(false);
    let r = write::write_image(&i, &mut dev, 32 << 20, None, &c, |p| { if p.phase == write::Phase::Write && p.done >= 4 << 20 { c.store(true, Ordering::Relaxed); } });
    assert_eq!(r.unwrap_err().code, "imager.cancelledWiped");
    fs::File::open(dir.path().join("device.bin")).unwrap().read_exact(&mut head).unwrap();
    assert!(head.iter().all(|&b| b == 0));
    // not an image name
    fs::write(dir.path().join("notes.txt"), b"x").unwrap();
    assert_eq!(write::inspect(&dir.path().join("notes.txt")).unwrap_err().code, "imager.badImageName");
    let _ = data;
}
