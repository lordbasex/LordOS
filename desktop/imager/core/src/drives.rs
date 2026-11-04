//! Removable drives, per platform. The rules are the native imager's: only removable drives (USB,
//! SD cards), never an internal disk and never the disk this computer runs from; the drive's
//! identity (size, model, serial) is checked again right before it is opened; nothing is mounted
//! on it while it's written.
//!
//! - `macos`: `diskutil` (physical disks: external ones, and the built-in SD card slot, which macOS calls
//!   internal but with removable media; the boot disk and the internal SSD are left out). The
//!   device is opened directly when the user may (a disk image the user attached for testing), or
//!   through the system's authorized-open helper, which asks for an administrator's approval and
//!   hands back only that one open device.
//! - `linux`: `lsblk` for the list; the device is opened through udisks2 (`OpenDevice`, approved by
//!   polkit like any desktop's "format disk"), so the app never runs as root.
//! - `windows`: `Get-Disk` (USB, SD and MMC bus types, never a boot or system disk); the app's
//!   manifest asks for administrator rights; volumes on the disk are locked and dismounted first.
use crate::{Error, Result};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Drive {
    /// stable id for this session: "disk4" (`macos`), "sdb" (`linux`), "2" (`windows` disk number)
    pub id: String,
    /// what to open: /dev/rdisk4, /dev/sdb, \\.\PhysicalDrive2
    pub path: String,
    pub model: String,
    pub serial: String,
    pub size: u64,
    pub transport: String,
    pub removable: bool,
    pub system: bool,
    pub read_only: bool,
    pub labels: Vec<String>,
    pub mounted: Vec<String>,
    /// a disk image (`macos`) or loop device (`linux`) attached for testing, only listed when its backing
    /// file is inside LORDOS_IMAGER_TEST_DEVICES=<folder>
    pub test_device: bool,
}

/// The identity the user picked must still be the drive's (a stick swapped under the same name is refused).
pub fn same_identity(a: &Drive, b: &Drive) -> bool {
    a.id == b.id && a.size == b.size && a.model == b.model && a.serial == b.serial && b.removable && !b.system
}

pub fn writable(d: &Drive) -> bool { d.removable && !d.system && !d.read_only && d.size <= MAX_DRIVE }

/// Testing only: also list disk images (`macos`) or loop devices (`linux`) whose backing file is inside
/// this folder (LORDOS_IMAGER_TEST_DEVICES=<absolute folder>). Images attached for anything else are
/// never listed, so a test run can't reach someone's other disk images.
pub fn test_dir() -> Option<std::path::PathBuf> {
    let v = std::env::var("LORDOS_IMAGER_TEST_DEVICES").ok()?;
    let p = std::path::PathBuf::from(v);
    if p.is_absolute() { p.canonicalize().ok() } else { None }
}
fn in_test_dir(backing: &str) -> bool {
    match (test_dir(), std::path::Path::new(backing).canonicalize()) { (Some(d), Ok(b)) => b.starts_with(d), _ => false }
}

/// LordOS images use an MBR partition table, which can't address more than 2 TiB: a larger drive is
/// refused (it is also very unlikely to be a card or a stick).
pub const MAX_DRIVE: u64 = 2 << 40;

/// `macos`: whether a physical disk may be listed. An external disk always may (a card reader, a stick);
/// an internal one only when its media is removable and it isn't the system's own media, which is how
/// `diskutil` describes a Mac's built-in SD card slot ("Device Location: Internal", "Removable Media:
/// Removable"). The internal SSD (fixed media, OS internal) never is. The boot disk is refused later.
#[allow(dead_code)]
pub(crate) fn mac_listable(internal: bool, removable_media: bool, os_internal: bool) -> bool {
    !internal || (removable_media && !os_internal)
}

#[allow(dead_code)]
fn run(cmd: &str, args: &[&str]) -> Result<String> {
    let out = std::process::Command::new(cmd).args(args).output().map_err(|e| Error::new("imager.listFailed", format!("{cmd}: {e}")))?;
    if !out.status.success() {
        return Err(Error::new("imager.listFailed", format!("{cmd}: {}", String::from_utf8_lossy(&out.stderr).trim())));
    }
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}

#[cfg(target_os = "macos")]
pub use mac::*;
#[cfg(target_os = "linux")]
pub use linux::*;
#[cfg(windows)]
pub use win::*;

// ---------------------------------------------------------------------------------------------
#[cfg(target_os = "macos")]
mod mac {
    use super::*;
    use std::fs::File;
    use std::os::fd::FromRawFd;

    fn info(dev: &str) -> Result<plist::Dictionary> {
        let out = run("/usr/sbin/diskutil", &["info", "-plist", dev])?;
        plist::from_bytes::<plist::Dictionary>(out.as_bytes()).map_err(|e| Error::new("imager.listFailed", e.to_string()))
    }
    fn s(d: &plist::Dictionary, k: &str) -> String { d.get(k).and_then(|v| v.as_string()).unwrap_or("").trim().to_string() }
    fn b(d: &plist::Dictionary, k: &str) -> bool { d.get(k).and_then(|v| v.as_boolean()).unwrap_or(false) }
    fn n(d: &plist::Dictionary, k: &str) -> u64 { d.get(k).and_then(|v| v.as_unsigned_integer()).unwrap_or(0) }

    /// The whole disk that holds "/" (and its APFS container's physical store).
    fn boot_disks() -> Vec<String> {
        let mut out = vec![];
        if let Ok(d) = info("/") {
            let w = s(&d, "ParentWholeDisk");
            if !w.is_empty() { out.push(w); }
            if let Some(stores) = d.get("APFSPhysicalStores").and_then(|v| v.as_array()) {
                for st in stores {
                    if let Some(id) = st.as_dictionary().map(|x| s(x, "APFSPhysicalStore")) {
                        if let Ok(pd) = info(&id) { out.push(s(&pd, "ParentWholeDisk")); }
                    }
                }
            }
        }
        out
    }

    /// disk image backing files by device (hdiutil info), for the test folder rule
    fn image_files() -> std::collections::HashMap<String, String> {
        let mut m = std::collections::HashMap::new();
        let Ok(out) = run("/usr/bin/hdiutil", &["info", "-plist"]) else { return m };
        let Ok(d) = plist::from_bytes::<plist::Dictionary>(out.as_bytes()) else { return m };
        for img in d.get("images").and_then(|v| v.as_array()).cloned().unwrap_or_default().iter().filter_map(|v| v.as_dictionary().cloned()) {
            let path = s(&img, "image-path");
            for e in img.get("system-entities").and_then(|v| v.as_array()).cloned().unwrap_or_default().iter().filter_map(|v| v.as_dictionary().cloned()) {
                let dev = s(&e, "dev-entry");
                if !dev.is_empty() { m.insert(dev.trim_start_matches("/dev/").to_string(), path.clone()); }
            }
        }
        m
    }

    pub fn list() -> Result<Vec<Drive>> {
        // physical, not only external: a Mac's built-in SD card slot is an internal disk with removable media
        let listing = run("/usr/sbin/diskutil", &["list", "-plist", "physical"])?;
        let l = plist::from_bytes::<plist::Dictionary>(listing.as_bytes()).map_err(|e| Error::new("imager.listFailed", e.to_string()))?;
        let boot = boot_disks();
        let images = if test_dir().is_some() { image_files() } else { Default::default() };
        let mut out = vec![];
        let whole = l.get("WholeDisks").and_then(|v| v.as_array()).cloned().unwrap_or_default();
        for w in whole.iter().filter_map(|v| v.as_string()) {
            let Ok(d) = info(w) else { continue };
            let virt = s(&d, "VirtualOrPhysical") == "Virtual";
            let image = s(&d, "BusProtocol") == "Disk Image";
            // APFS containers and other synthesized disks are not drives; a disk image only in a test run
            if virt && !(image && images.get(w).is_some_and(|f| in_test_dir(f))) { continue; }
            if !image && !super::mac_listable(b(&d, "Internal"), b(&d, "RemovableMedia") || b(&d, "Removable"), b(&d, "OSInternalMedia")) { continue; }
            let id = s(&d, "DeviceIdentifier");
            let mut labels = vec![];
            let mut mounted = vec![];
            if let Some(all) = l.get("AllDisksAndPartitions").and_then(|v| v.as_array()) {
                for e in all.iter().filter_map(|v| v.as_dictionary()) {
                    if s(e, "DeviceIdentifier") != id { continue; }
                    for p in e.get("Partitions").and_then(|v| v.as_array()).cloned().unwrap_or_default().iter().filter_map(|v| v.as_dictionary().cloned()) {
                        let vn = s(&p, "VolumeName");
                        if !vn.is_empty() { labels.push(vn.clone()); }
                        let mp = s(&p, "MountPoint");
                        if !mp.is_empty() { mounted.push(if vn.is_empty() { mp } else { vn }); }
                    }
                }
            }
            let removable = b(&d, "RemovableMedia") || b(&d, "Removable") || b(&d, "Ejectable") || s(&d, "BusProtocol") == "USB" || s(&d, "BusProtocol") == "Secure Digital";
            out.push(Drive {
                path: format!("/dev/r{id}"), model: s(&d, "MediaName"), serial: s(&d, "DiskUUID"), size: n(&d, "Size"),
                transport: match s(&d, "BusProtocol").as_str() { "USB" => "usb".into(), "Secure Digital" => "sd".into(), "Disk Image" => "image".into(), o => o.to_lowercase() },
                removable, system: boot.contains(&id) || b(&d, "OSInternalMedia"), read_only: !b(&d, "WritableMedia"),
                labels, mounted, test_device: image, id,
            });
        }
        Ok(out)
    }

    pub fn unmount(d: &Drive) -> Result<()> {
        run("/usr/sbin/diskutil", &["unmountDisk", &format!("/dev/{}", d.id)]).map(|_| ()).map_err(|e| Error::new("disk.busy", e.detail))
    }

    /// Opens the raw device: directly if the user may, otherwise through the system's authorized
    /// open helper (an administrator approves it in the system's own dialog).
    pub fn open(d: &Drive) -> Result<File> {
        use std::fs::OpenOptions;
        match OpenOptions::new().read(true).write(true).open(&d.path) {
            Ok(f) => return Ok(f),
            Err(e) if e.kind() != std::io::ErrorKind::PermissionDenied => return Err(Error::new("disk.openFailed", e.to_string())),
            Err(_) => {}
        }
        authopen(&d.path)
    }

    fn authopen(path: &str) -> Result<File> {
        // authopen -stdoutpipe sends the opened descriptor over its stdout, which must be a socket
        let mut sv = [0i32; 2];
        if unsafe { libc::socketpair(libc::AF_UNIX, libc::SOCK_STREAM, 0, sv.as_mut_ptr()) } != 0 {
            return Err(Error::new("disk.openFailed", "socketpair"));
        }
        let child = unsafe { std::process::Stdio::from_raw_fd(sv[1]) };
        let st = std::process::Command::new("/usr/libexec/authopen")
            .args(["-stdoutpipe", "-o", &(libc::O_RDWR).to_string(), path]).stdout(child).spawn();
        let mut proc = st.map_err(|e| Error::new("disk.openFailed", e.to_string()))?;
        let mut buf = [0u8; 64];
        let mut cmsg = [0u8; 64];
        let mut iov = libc::iovec { iov_base: buf.as_mut_ptr() as *mut _, iov_len: buf.len() };
        let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
        msg.msg_iov = &mut iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg.as_mut_ptr() as *mut _;
        msg.msg_controllen = cmsg.len() as _;
        let r = unsafe { libc::recvmsg(sv[0], &mut msg, 0) };
        let _ = proc.wait();
        unsafe { libc::close(sv[0]) };
        if r <= 0 { return Err(Error::new("imager.notAuthorized", "")); }
        let c = unsafe { libc::CMSG_FIRSTHDR(&msg) };
        if c.is_null() || unsafe { (*c).cmsg_type } != libc::SCM_RIGHTS { return Err(Error::new("imager.notAuthorized", "no descriptor")); }
        let fd = unsafe { *(libc::CMSG_DATA(c) as *const i32) };
        Ok(unsafe { File::from_raw_fd(fd) })
    }

    pub fn eject(d: &Drive) -> Result<()> {
        run("/usr/sbin/diskutil", &["eject", &format!("/dev/{}", d.id)]).map(|_| ())
    }

    /// Mounts the drive's first partition (the boot partition) and returns where it is.
    pub fn mount_boot(d: &Drive) -> Result<std::path::PathBuf> {
        let part = format!("/dev/{}s1", d.id);
        for _ in 0..30 {
            let _ = run("/usr/sbin/diskutil", &["mount", &part]);
            if let Ok(i) = info(&part) {
                let mp = s(&i, "MountPoint");
                if !mp.is_empty() { return Ok(mp.into()); }
            }
            std::thread::sleep(std::time::Duration::from_millis(500));
        }
        Err(Error::new("imager.bootNotFound", part))
    }
    pub fn unmount_boot(d: &Drive) { let _ = run("/usr/sbin/diskutil", &["unmount", &format!("/dev/{}s1", d.id)]); }
}

// ---------------------------------------------------------------------------------------------
#[cfg(target_os = "linux")]
mod linux {
    use super::*;
    use std::collections::HashMap;
    use std::fs::File;

    const SYSTEM: &[&str] = &["/", "/boot", "/boot/efi", "/efi", "/usr", "/var", "/home", "[SWAP]", "/run/live/medium", "/cdrom"];

    pub fn list() -> Result<Vec<Drive>> {
        let out = run("lsblk", &["-J", "-b", "-o", "NAME,PATH,SIZE,RM,HOTPLUG,TRAN,TYPE,MODEL,VENDOR,SERIAL,RO,MOUNTPOINTS,LABEL"])?;
        let v: serde_json::Value = serde_json::from_str(&out).map_err(|e| Error::new("imager.listFailed", e.to_string()))?;
        let mut res = vec![];
        for d in v["blockdevices"].as_array().cloned().unwrap_or_default() {
            let ty = d["type"].as_str().unwrap_or("");
            let name = d["name"].as_str().unwrap_or("").to_string();
            let test = ty == "loop" && std::fs::read_to_string(format!("/sys/block/{name}/loop/backing_file")).is_ok_and(|f| in_test_dir(f.trim()));
            if ty != "disk" && !test { continue; }
            if name.starts_with("zram") || name.starts_with("sr") { continue; }
            let mut mounted = vec![];
            let mut labels = vec![];
            let mut system = false;
            let mut walk = vec![d.clone()];
            while let Some(n) = walk.pop() {
                for m in n["mountpoints"].as_array().cloned().unwrap_or_default().iter().filter_map(|m| m.as_str().map(String::from)) {
                    if SYSTEM.contains(&m.as_str()) { system = true; }
                    mounted.push(m);
                }
                if let Some(l) = n["label"].as_str() { labels.push(l.to_string()); }
                for c in n["children"].as_array().cloned().unwrap_or_default() { walk.push(c); }
            }
            let tran = d["tran"].as_str().unwrap_or("").to_string();
            let b = |k: &str| d[k].as_bool().unwrap_or(false) || d[k].as_str() == Some("1");
            let removable = b("rm") || b("hotplug") || tran == "usb" || tran == "mmc" || name.starts_with("mmcblk") || test;
            res.push(Drive {
                id: name.clone(), path: d["path"].as_str().map(String::from).unwrap_or(format!("/dev/{name}")),
                model: format!("{} {}", d["vendor"].as_str().unwrap_or("").trim(), d["model"].as_str().unwrap_or("").trim()).trim().to_string(),
                serial: d["serial"].as_str().unwrap_or("").trim().to_string(), size: d["size"].as_u64().unwrap_or(0),
                transport: if tran == "mmc" || name.starts_with("mmcblk") { "sd".into() } else { tran },
                removable, system, read_only: b("ro"), labels, mounted, test_device: test,
            });
        }
        Ok(res)
    }

    // ---- udisks2 over D-Bus (the system bus; polkit asks for approval when needed) ----
    fn udisks_path(dev: &str) -> String {
        let name = dev.trim_start_matches("/dev/");
        format!("/org/freedesktop/UDisks2/block_devices/{}", name.replace('-', "_2d"))
    }
    fn conn() -> Result<zbus::blocking::Connection> { zbus::blocking::Connection::system().map_err(|e| Error::new("imager.noUdisks", e.to_string())) }
    fn opts() -> HashMap<&'static str, zbus::zvariant::Value<'static>> {
        let mut m = HashMap::new();
        m.insert("auth.no_user_interaction", zbus::zvariant::Value::Bool(false));
        m
    }

    pub fn unmount(d: &Drive) -> Result<()> {
        let c = conn()?;
        let parts = run("lsblk", &["-ln", "-o", "PATH,MOUNTPOINTS", &d.path])?;
        for line in parts.lines() {
            let mut it = line.split_whitespace();
            let (Some(p), Some(_m)) = (it.next(), it.next()) else { continue };
            let op = udisks_path(p);
            let proxy = zbus::blocking::Proxy::new(&c, "org.freedesktop.UDisks2", op.as_str(), "org.freedesktop.UDisks2.Filesystem").map_err(|e| Error::new("disk.busy", e.to_string()))?;
            proxy.call_method("Unmount", &(opts(),)).map_err(|e| Error::new("disk.busy", e.to_string()))?;
        }
        Ok(())
    }

    pub fn open(d: &Drive) -> Result<File> {
        use std::os::fd::{FromRawFd, IntoRawFd};
        if let Ok(f) = std::fs::OpenOptions::new().read(true).write(true).open(&d.path) { return Ok(f); }    // root, or a test loop device
        let c = conn()?;
        let op = udisks_path(&d.path);
        let proxy = zbus::blocking::Proxy::new(&c, "org.freedesktop.UDisks2", op.as_str(), "org.freedesktop.UDisks2.Block").map_err(|e| Error::new("disk.openFailed", e.to_string()))?;
        let mut o = opts();
        o.insert("flags", zbus::zvariant::Value::I32(libc::O_EXCL));
        let reply = proxy.call_method("OpenDevice", &("rw", o)).map_err(|e| Error::new("imager.notAuthorized", e.to_string()))?;
        let fd: zbus::zvariant::OwnedFd = reply.body().deserialize().map_err(|e| Error::new("disk.openFailed", e.to_string()))?;
        let raw = std::os::fd::OwnedFd::from(fd).into_raw_fd();
        Ok(unsafe { File::from_raw_fd(raw) })
    }

    pub fn eject(d: &Drive) -> Result<()> {
        let c = conn()?;
        // the drive object behind the block device, then PowerOff (like "Safely remove")
        let op = udisks_path(&d.path);
        let block = zbus::blocking::Proxy::new(&c, "org.freedesktop.UDisks2", op.as_str(), "org.freedesktop.UDisks2.Block").map_err(|e| Error::new("imager.ejectFailed", e.to_string()))?;
        let drive: zbus::zvariant::OwnedObjectPath = block.get_property("Drive").map_err(|e| Error::new("imager.ejectFailed", e.to_string()))?;
        let dp = zbus::blocking::Proxy::new(&c, "org.freedesktop.UDisks2", drive.as_str(), "org.freedesktop.UDisks2.Drive").map_err(|e| Error::new("imager.ejectFailed", e.to_string()))?;
        // Eject takes the medium out (a card reader, a stick); PowerOff then cuts a USB drive's power.
        // Either one is enough for the drive to be removed safely.
        let ejected = dp.call_method("Eject", &(opts(),));
        let off = dp.call_method("PowerOff", &(opts(),));
        if ejected.is_ok() || off.is_ok() { Ok(()) } else { Err(Error::new("imager.ejectFailed", off.err().map(|e| e.to_string()).unwrap_or_default())) }
    }

    fn part1(d: &Drive) -> String { if d.path.chars().last().is_some_and(|c| c.is_ascii_digit()) { format!("{}p1", d.path) } else { format!("{}1", d.path) } }

    pub fn mount_boot(d: &Drive) -> Result<std::path::PathBuf> {
        let p = part1(d);
        let c = conn()?;
        for _ in 0..30 {
            if std::path::Path::new(&p).exists() {
                let op = udisks_path(&p);
                let fs = zbus::blocking::Proxy::new(&c, "org.freedesktop.UDisks2", op.as_str(), "org.freedesktop.UDisks2.Filesystem").map_err(|e| Error::new("imager.bootNotFound", e.to_string()))?;
                if let Ok(r) = fs.call_method("Mount", &(opts(),)) {
                    let mp: String = r.body().deserialize().map_err(|e| Error::new("imager.bootNotFound", e.to_string()))?;
                    return Ok(mp.into());
                }
            }
            std::thread::sleep(std::time::Duration::from_millis(500));
        }
        Err(Error::new("imager.bootNotFound", p))
    }
    pub fn unmount_boot(d: &Drive) {
        if let Ok(c) = conn() {
            let op = udisks_path(&part1(d));
            let fs = zbus::blocking::Proxy::new(&c, "org.freedesktop.UDisks2", op.as_str(), "org.freedesktop.UDisks2.Filesystem");
            if let Ok(fs) = fs { let _ = fs.call_method("Unmount", &(opts(),)); }
        }
    }
}

// ---------------------------------------------------------------------------------------------
#[cfg(windows)]
mod win {
    use super::*;
    use std::fs::File;
    use std::os::windows::io::FromRawHandle;
    use windows_sys::Win32::Foundation::*;
    use windows_sys::Win32::Storage::FileSystem::*;
    use windows_sys::Win32::System::Ioctl::*;
    use windows_sys::Win32::System::IO::DeviceIoControl;

    /// The disk number, parsed strictly: it is the only value put into a PowerShell command.
    fn num(d: &Drive) -> Result<u32> {
        if d.id.is_empty() || d.id.len() > 4 || !d.id.bytes().all(|b| b.is_ascii_digit()) { return Err(Error::new("admin.invalidArgument", "")); }
        d.id.parse().map_err(|_| Error::new("admin.invalidArgument", ""))
    }

    fn ps(script: &str) -> Result<String> {
        run("powershell.exe", &["-NoProfile", "-NonInteractive", "-Command", script])
    }

    pub fn list() -> Result<Vec<Drive>> {
        let out = ps("Get-Disk | Select-Object Number,FriendlyName,SerialNumber,Size,BusType,IsBoot,IsSystem,IsReadOnly | ConvertTo-Json -Compress")?;
        let v: serde_json::Value = serde_json::from_str(out.trim()).map_err(|e| Error::new("imager.listFailed", e.to_string()))?;
        let arr = if v.is_array() { v.as_array().cloned().unwrap_or_default() } else { vec![v] };
        let mut res = vec![];
        for d in arr {
            let num = d["Number"].as_u64().unwrap_or(u64::MAX);
            if num == u64::MAX { continue; }
            let bus = d["BusType"].as_str().map(String::from).unwrap_or_else(|| d["BusType"].as_u64().map(|n| n.to_string()).unwrap_or_default());
            // BusType: 7 = USB, 12 = SD, 13 = MMC (MSFT_Disk); names when PowerShell prints them
            let removable = matches!(bus.as_str(), "USB" | "SD" | "MMC" | "7" | "12" | "13");
            res.push(Drive {
                id: num.to_string(), path: format!(r"\\.\PhysicalDrive{num}"),
                model: d["FriendlyName"].as_str().unwrap_or("").trim().to_string(), serial: d["SerialNumber"].as_str().unwrap_or("").trim().to_string(),
                size: d["Size"].as_u64().unwrap_or(0), transport: if matches!(bus.as_str(), "SD" | "MMC" | "12" | "13") { "sd".into() } else { "usb".into() },
                removable, system: d["IsBoot"].as_bool().unwrap_or(false) || d["IsSystem"].as_bool().unwrap_or(false),
                read_only: d["IsReadOnly"].as_bool().unwrap_or(false), labels: vec![], mounted: vec![], test_device: false,
            });
        }
        Ok(res)
    }

    /// Each volume on the disk is locked and dismounted; the handles stay open (and locked) while
    /// the disk is written, in this process.
    static LOCKS: std::sync::Mutex<Vec<isize>> = std::sync::Mutex::new(Vec::new());

    pub fn unmount(d: &Drive) -> Result<()> {
        let letters = ps(&format!("(Get-Partition -DiskNumber {} | Where-Object DriveLetter | ForEach-Object {{ $_.DriveLetter }}) -join ','", num(d)?))?;
        for l in letters.trim().split(',').filter(|s| s.len() == 1) {
            let path: Vec<u16> = format!(r"\\.\{l}:").encode_utf16().chain([0]).collect();
            let h = unsafe { CreateFileW(path.as_ptr(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, std::ptr::null(), OPEN_EXISTING, 0, std::ptr::null_mut()) };
            if h == INVALID_HANDLE_VALUE { return Err(Error::new("disk.busy", format!("{l}:"))); }
            let mut n = 0u32;
            let ok = unsafe { DeviceIoControl(h, FSCTL_LOCK_VOLUME, std::ptr::null(), 0, std::ptr::null_mut(), 0, &mut n, std::ptr::null_mut()) } != 0
                && unsafe { DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, std::ptr::null(), 0, std::ptr::null_mut(), 0, &mut n, std::ptr::null_mut()) } != 0;
            if !ok { unsafe { CloseHandle(h) }; return Err(Error::new("disk.busy", format!("{l}:"))); }
            LOCKS.lock().unwrap().push(h as isize);
        }
        Ok(())
    }

    pub fn open(d: &Drive) -> Result<File> {
        let path: Vec<u16> = d.path.encode_utf16().chain([0]).collect();
        let h = unsafe { CreateFileW(path.as_ptr(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, std::ptr::null(), OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, std::ptr::null_mut()) };
        if h == INVALID_HANDLE_VALUE { return Err(Error::new("imager.notAuthorized", d.path.clone())); }
        Ok(unsafe { File::from_raw_handle(h as _) })
    }

    pub fn release_locks() {
        for h in LOCKS.lock().unwrap().drain(..) { unsafe { CloseHandle(h as _) }; }
    }

    pub fn eject(d: &Drive) -> Result<()> {
        release_locks();
        ps(&format!("$s = New-Object -ComObject Shell.Application; Get-Partition -DiskNumber {} | Where-Object DriveLetter | ForEach-Object {{ $s.Namespace(17).ParseName(\"$($_.DriveLetter):\").InvokeVerb('Eject') }}", num(d)?)).map(|_| ())
    }

    pub fn mount_boot(d: &Drive) -> Result<std::path::PathBuf> {
        release_locks();
        let n = num(d)?;
        let _ = ps(&format!("Update-Disk -Number {n}"));
        for _ in 0..30 {
            let l = ps(&format!("$p = Get-Partition -DiskNumber {} -PartitionNumber 1 -ErrorAction SilentlyContinue; if ($p -and -not $p.DriveLetter) {{ $p | Add-PartitionAccessPath -AssignDriveLetter | Out-Null; $p = Get-Partition -DiskNumber {} -PartitionNumber 1 }}; $p.DriveLetter", n, n)).unwrap_or_default();
            let l = l.trim();
            if l.len() == 1 { return Ok(format!(r"{l}:\").into()); }
            std::thread::sleep(std::time::Duration::from_millis(500));
        }
        Err(Error::new("imager.bootNotFound", d.id.clone()))
    }
    pub fn unmount_boot(_d: &Drive) {}
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mac_sd_slot_is_listed_and_the_internal_ssd_is_not() {
        // a USB card reader or stick
        assert!(mac_listable(false, true, false));
        assert!(mac_listable(false, false, false));
        // the built-in SD card slot: internal, removable media, not the OS's own media
        assert!(mac_listable(true, true, false));
        // the internal SSD: internal, fixed media, OS internal
        assert!(!mac_listable(true, false, true));
        assert!(!mac_listable(true, false, false));
        assert!(!mac_listable(true, true, true));
    }
}
