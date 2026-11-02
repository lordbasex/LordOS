//! LordOS Imager for the desktop (`macos`, `windows`, `linux`): the Tauri shell. The window shows the same React UI as
//! the native app (apps/imager, built by desktop/imager/build-web.mjs); its platform module calls
//! the commands below. Nothing here runs a shell; downloads go only to the LordOS releases on GitHub,
//! and writing goes through lordos-imager-core. docs/IMAGER.md, "The desktop app".
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use lordos_imager_core::{drives, secrets, sums, write, Error, RELEASE_KEY};
use serde::Serialize;
use serde_json::{json, Value};
use std::fs;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use tauri::{AppHandle, Emitter, Manager, State};

const REPO: &str = "lordbasex/LordOS";
const UA: &str = concat!("LordOS-Imager/", env!("CARGO_PKG_VERSION"));

struct App {
    cancel: Arc<AtomicBool>,
    cache: Mutex<PathBuf>,
    config: PathBuf,
}

type Res<T> = Result<T, Value>;
fn err(e: Error) -> Value { json!({ "code": e.code, "detail": e.detail }) }
fn code(c: &'static str, d: impl Into<String>) -> Value { err(Error::new(c, d)) }

fn config_dir() -> PathBuf { dirs::config_dir().unwrap_or_else(std::env::temp_dir).join("LordOS Imager") }
fn default_cache() -> PathBuf { dirs::cache_dir().unwrap_or_else(std::env::temp_dir).join("LordOS Imager") }

fn read_json(p: &Path) -> Value { fs::read(p).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or(Value::Null) }
fn write_json(p: &Path, v: &Value) -> std::io::Result<()> {
    if let Some(d) = p.parent() { fs::create_dir_all(d)?; }
    let tmp = p.with_extension("json.new");
    fs::write(&tmp, serde_json::to_vec_pretty(v)?)?;
    fs::rename(tmp, p)
}

// ---- context ----

#[derive(Serialize)]
struct Platform { os: &'static str, arch: &'static str, version: &'static str, lang: String, timezone: String, country: String, cache: String,
                  #[serde(rename = "testState")] test_state: Value }

/// Tests and documentation screenshots only: LORDOS_IMAGER_TEST_STATE=<file.json> starts the window
/// at a given step with given choices (and may start the write, but only to a test device: one the
/// LORDOS_IMAGER_TEST_DEVICES folder rule lists). Never set by the app itself.
fn test_state() -> Value {
    let Ok(p) = std::env::var("LORDOS_IMAGER_TEST_STATE") else { return Value::Null };
    match fs::metadata(&p) { Ok(m) if m.is_file() && m.len() < 65536 => read_json(Path::new(&p)), _ => Value::Null }
}

#[tauri::command]
fn platform(st: State<App>) -> Platform {
    let loc = sys_locale::get_locale().unwrap_or_default();
    let lang = loc.split(['-', '_']).next().unwrap_or("en").to_lowercase();
    let country = loc.split(['-', '_']).nth(1).unwrap_or("").to_uppercase();
    Platform {
        os: std::env::consts::OS, arch: std::env::consts::ARCH, version: env!("CARGO_PKG_VERSION"),
        lang: if ["en", "es", "pt", "fr"].contains(&lang.as_str()) { lang } else { "en".into() },
        timezone: iana_time_zone::get_timezone().unwrap_or_default(),
        country: if country.len() == 2 && country.bytes().all(|c| c.is_ascii_uppercase()) { country } else { String::new() },
        cache: st.cache.lock().unwrap().display().to_string(),
        test_state: test_state(),
    }
}

// ---- drives ----

#[tauri::command]
async fn list_drives() -> Res<Vec<drives::Drive>> {
    tauri::async_runtime::spawn_blocking(|| drives::list().map_err(err)).await.map_err(|e| code("imager.listFailed", e.to_string()))?
}

// ---- releases and downloads (only GitHub's LordOS releases; the signature decides what is trusted) ----

fn client() -> Res<reqwest::blocking::Client> {
    reqwest::blocking::Client::builder().user_agent(UA).https_only(true).connect_timeout(std::time::Duration::from_secs(20))
        .build().map_err(|e| code("imager.fetchNetwork", e.to_string()))
}

#[tauri::command]
async fn fetch_releases(st: State<'_, App>) -> Res<String> {
    let cache = st.cache.lock().unwrap().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let url = format!("https://api.github.com/repos/{REPO}/releases?per_page=10");
        let r = client()?.get(url).header("Accept", "application/vnd.github+json").send().map_err(|e| code("imager.fetchNetwork", e.to_string()))?;
        if !r.status().is_success() { return Err(code("imager.fetchFailed", r.status().as_str().to_string())); }
        let text = r.text().map_err(|e| code("imager.fetchNetwork", e.to_string()))?;
        if text.len() > 4 << 20 { return Err(code("imager.fetchTooLarge", "")); }
        let _ = fs::create_dir_all(&cache);
        let _ = fs::write(cache.join("releases.json"), &text);
        Ok(text)
    }).await.map_err(|e| code("imager.fetchFailed", e.to_string()))?
}

#[tauri::command]
fn cached_releases(st: State<App>) -> Option<String> { fs::read_to_string(st.cache.lock().unwrap().join("releases.json")).ok() }

fn asset_path(st: &App, tag: &str, name: &str) -> Res<PathBuf> {
    if !lordos_imager_core::valid_tag(tag) || !lordos_imager_core::valid_name(name) { return Err(code("admin.invalidArgument", "")); }
    Ok(st.cache.lock().unwrap().join(tag).join(name))
}

#[tauri::command]
fn cache_state(st: State<App>, tag: String, name: String, size: u64) -> Res<Value> {
    let p = asset_path(&st, &tag, &name)?;
    let cached = fs::metadata(&p).map(|m| m.len() == size).unwrap_or(false);
    let partial = fs::metadata(part_of(&p)).map(|m| m.len()).unwrap_or(0);
    Ok(json!({ "cached": cached, "partial": partial, "path": p.display().to_string() }))
}

fn part_of(p: &Path) -> PathBuf { let mut s = p.as_os_str().to_owned(); s.push(".part"); PathBuf::from(s) }

#[tauri::command]
async fn download(app: AppHandle, st: State<'_, App>, tag: String, name: String, size: u64) -> Res<String> {
    let dest = asset_path(&st, &tag, &name)?;
    let cancel = st.cancel.clone();
    cancel.store(false, Ordering::Relaxed);
    if size == 0 || size > 64 << 30 { return Err(code("admin.invalidArgument", "size")); }
    tauri::async_runtime::spawn_blocking(move || {
        if fs::metadata(&dest).map(|m| m.len() == size).unwrap_or(false) { return Ok(dest.display().to_string()); }
        fs::create_dir_all(dest.parent().unwrap()).map_err(|e| code("imager.cacheFailed", e.to_string()))?;
        let part = part_of(&dest);
        let have = fs::metadata(&part).map(|m| m.len()).unwrap_or(0);
        let have = if have > size { let _ = fs::remove_file(&part); 0 } else { have };
        let url = format!("https://github.com/{REPO}/releases/download/{tag}/{name}");
        let mut req = client()?.get(url);
        if have > 0 { req = req.header("Range", format!("bytes={have}-")); }
        let mut r = req.send().map_err(|e| code("imager.fetchNetwork", e.to_string()))?;
        let resumed = r.status().as_u16() == 206;
        if r.status().as_u16() == 404 { return Err(code("imager.fetchNotFound", "")); }
        if !r.status().is_success() { return Err(code("imager.fetchFailed", r.status().as_str().to_string())); }
        let mut f = fs::OpenOptions::new().create(true).append(resumed).write(true).truncate(!resumed).open(&part).map_err(|e| code("imager.cacheFailed", e.to_string()))?;
        let mut done = if resumed { have } else { 0 };
        let mut buf = vec![0u8; 256 << 10];
        let mut last = std::time::Instant::now();
        loop {
            if cancel.load(Ordering::Relaxed) { return Err(code("imager.fetchCancelled", "")); }
            let n = r.read(&mut buf).map_err(|e| code("imager.fetchNetwork", e.to_string()))?;
            if n == 0 { break; }
            done += n as u64;
            if done > size { let _ = fs::remove_file(&part); return Err(code("imager.fetchTooLarge", "")); }
            f.write_all(&buf[..n]).map_err(|e| code("imager.cacheFailed", e.to_string()))?;
            if last.elapsed().as_millis() > 300 {
                last = std::time::Instant::now();
                let _ = app.emit("imager-progress", json!({ "phase": "download", "done": done, "total": size }));
            }
        }
        f.sync_all().ok();
        if done != size { return Err(code("imager.fetchFailed", format!("{done} of {size} bytes"))); }
        fs::rename(&part, &dest).map_err(|e| code("imager.cacheFailed", e.to_string()))?;
        Ok(dest.display().to_string())
    }).await.map_err(|e| code("imager.fetchFailed", e.to_string()))?
}

#[tauri::command]
fn cancel(st: State<App>) { st.cancel.store(true, Ordering::Relaxed); }

/// Only the small text files of the cache (SHA256SUMS, its signature) are read back by the UI.
#[tauri::command]
fn read_cache_text(st: State<App>, path: String) -> Res<String> {
    let cache = st.cache.lock().unwrap().canonicalize().map_err(|e| code("fs.readFailed", e.to_string()))?;
    let p = PathBuf::from(&path).canonicalize().map_err(|e| code("fs.readFailed", e.to_string()))?;
    if !p.starts_with(&cache) || fs::metadata(&p).map(|m| m.len() > 4 << 20).unwrap_or(true) { return Err(code("fs.readFailed", "")); }
    fs::read_to_string(p).map_err(|e| code("fs.readFailed", e.to_string()))
}

#[tauri::command]
fn check_sums(sums_text: String, sig: String) -> Value {
    if sums::verify(sums_text.as_bytes(), sig.as_bytes(), &RELEASE_KEY).is_err() { return json!({ "ok": false }); }
    match sums::parse(sums_text.as_bytes()) {
        Ok(m) => json!({ "ok": true, "files": m.iter().map(|(k, v)| (k.clone(), Value::String(sums::hex(v)))).collect::<serde_json::Map<_, _>>() }),
        Err(_) => json!({ "ok": false }),
    }
}

// ---- the user's own files ----

#[tauri::command]
fn image_info(path: String) -> Res<Value> {
    let i = write::inspect(Path::new(&path)).map_err(err)?;
    Ok(json!({ "kind": if i.xz { "xz" } else { "raw" }, "size": i.file_size, "raw": i.raw_size, "path": path,
               "name": Path::new(&path).file_name().and_then(|n| n.to_str()).unwrap_or("") }))
}

#[tauri::command]
fn read_key_file(path: String) -> Res<String> {
    let m = fs::metadata(&path).map_err(|e| code("fs.readFailed", e.to_string()))?;
    if !m.is_file() || m.len() > 64 << 10 { return Err(code("fs.readFailed", "")); }
    fs::read_to_string(path).map_err(|e| code("fs.readFailed", e.to_string()))
}

// ---- secrets: derived here; only the results go to the card ----

#[tauri::command]
async fn hash_password(password: String) -> Res<String> {
    tauri::async_runtime::spawn_blocking(move || secrets::hash_password(&password).map_err(err)).await.map_err(|e| code("imager.hashFailed", e.to_string()))?
}

#[tauri::command]
fn wifi_psk(ssid: String, password: String) -> Res<String> { secrets::wifi_psk(&ssid, &password).map_err(err) }

// ---- writing ----

fn find_drive(want: &drives::Drive) -> Res<drives::Drive> {
    let now = drives::list().map_err(err)?;
    let d = now.into_iter().find(|d| drives::same_identity(want, d)).ok_or_else(|| code("imager.diskChanged", want.id.clone()))?;
    if !drives::writable(&d) { return Err(code(if d.read_only { "imager.readOnly" } else { "imager.notRemovable" }, d.id.clone())); }
    if d.test_device != want.test_device { return Err(code("imager.diskChanged", d.id.clone())); }
    Ok(d)
}

#[tauri::command]
async fn write_image(app: AppHandle, st: State<'_, App>, drive: drives::Drive, path: String, official: bool, sums_path: Option<String>, sig_path: Option<String>) -> Res<write::Outcome> {
    let cancel = st.cancel.clone();
    cancel.store(false, Ordering::Relaxed);
    tauri::async_runtime::spawn_blocking(move || {
        // 1. the drive: removable, not the system disk, and still the one the user picked
        let d = find_drive(&drive)?;
        let img = write::inspect(Path::new(&path)).map_err(err)?;
        if img.raw_size > d.size { return Err(code("disk.imageTooLarge", format!("{} > {}", img.raw_size, d.size))); }
        // 2. official: signature and listed hash before anything is erased
        let listed = if official {
            let s = fs::read(sums_path.as_deref().unwrap_or("")).map_err(|_| code("imager.noSignature", ""))?;
            let g = fs::read(sig_path.as_deref().unwrap_or("")).map_err(|_| code("imager.noSignature", ""))?;
            sums::verify(&s, &g, &RELEASE_KEY).map_err(err)?;
            let name = img.path.file_name().and_then(|n| n.to_str()).unwrap_or("").to_string();
            let h = *sums::parse(&s).map_err(err)?.get(&name).ok_or_else(|| code("imager.notListed", name.clone()))?;
            let got = write::sha256_file(&img.path, &cancel, |p| { let _ = app.emit("imager-progress", &p); }).map_err(err)?;
            if got != h { return Err(code("imager.hashMismatch", "")); }
            Some(h)
        } else { None };
        // 3. right before opening: the same drive again, nothing of it mounted, opened for writing
        let d = find_drive(&d)?;
        drives::unmount(&d).map_err(err)?;
        let mut dev = drives::open(&d).map_err(err)?;
        eprintln!("lordos-imager: writing {} ({} bytes) to {} ({})", img.path.display(), img.raw_size, d.path, d.model);
        let r = write::write_image(&img, &mut dev, d.size, listed, &cancel, |p| { let _ = app.emit("imager-progress", &p); });
        match &r { Ok(o) => eprintln!("lordos-imager: written and verified, sha256 {}", o.sha256), Err(e) => eprintln!("lordos-imager: write failed: {e}") }
        drop(dev);
        #[cfg(windows)]
        if r.is_err() { drives::release_locks(); }
        r.map_err(err)
    }).await.map_err(|e| code("disk.writeFailed", e.to_string()))?
}

/// The first-run file onto the card's boot partition (mounted for the user, then unmounted).
#[tauri::command]
async fn write_firstrun(drive: drives::Drive, text: String, name: String) -> Res<()> {
    if name != "lordos-firstrun.txt" || text.len() > 65536 { return Err(code("admin.invalidArgument", "")); }
    tauri::async_runtime::spawn_blocking(move || {
        // the drive as listed now, never the fields the page sent
        let d = find_drive(&drive)?;
        let dir = drives::mount_boot(&d).map_err(err)?;
        let r = fs::write(dir.join(&name), text.as_bytes()).and_then(|_| fs::File::open(dir.join(&name))?.sync_all());
        drives::unmount_boot(&d);
        r.map_err(|e| code("fs.writeFailed", e.to_string()))
    }).await.map_err(|e| code("fs.writeFailed", e.to_string()))?
}

#[tauri::command]
async fn eject(drive: drives::Drive) -> Res<()> {
    tauri::async_runtime::spawn_blocking(move || { let d = find_drive(&drive)?; drives::eject(&d).map_err(err) }).await.map_err(|e| code("imager.ejectFailed", e.to_string()))?
}

// ---- options, profiles (never secrets), cache ----

#[tauri::command]
fn load_options(st: State<App>) -> Value { read_json(&st.config.join("options.json")) }
#[tauri::command]
fn save_options(st: State<App>, options: Value) -> Res<()> {
    let mut cur = read_json(&st.config.join("options.json"));
    if !cur.is_object() { cur = json!({}); }
    if let (Some(c), Some(o)) = (cur.as_object_mut(), options.as_object()) { for (k, v) in o { c.insert(k.clone(), v.clone()); } }
    if let Some(dir) = cur.get("cacheDir").and_then(|v| v.as_str()) { *st.cache.lock().unwrap() = PathBuf::from(dir); }
    write_json(&st.config.join("options.json"), &cur).map_err(|e| code("fs.writeFailed", e.to_string()))
}
#[tauri::command]
fn load_profiles(st: State<App>) -> Value { let v = read_json(&st.config.join("profiles.json")); if v.is_array() { v } else { json!([]) } }
#[tauri::command]
fn save_profiles(st: State<App>, profiles: Value) -> Res<()> {
    // profiles are written by the shared UI (core/firstrun.js toProfile) and hold no secrets; refuse anything that looks like one
    let text = profiles.to_string();
    if !profiles.is_array() || text.len() > 1 << 20 || text.contains("password_hash") || text.contains("\"psk\"") { return Err(code("admin.invalidArgument", "")); }
    write_json(&st.config.join("profiles.json"), &profiles).map_err(|e| code("fs.writeFailed", e.to_string()))
}

fn dir_size(p: &Path) -> u64 {
    fs::read_dir(p).map(|rd| rd.flatten().map(|e| {
        let m = e.metadata().ok();
        match m { Some(m) if m.is_dir() => dir_size(&e.path()), Some(m) => m.len(), None => 0 }
    }).sum()).unwrap_or(0)
}
#[tauri::command]
fn cache_size(st: State<App>) -> u64 { dir_size(&st.cache.lock().unwrap()) }
#[tauri::command]
fn clear_cache(st: State<App>) -> Res<()> {
    let c = st.cache.lock().unwrap().clone();
    // only what the imager puts there: releases.json and the release folders (v1.2.3-…)
    for e in fs::read_dir(&c).map_err(|e| code("imager.cacheFailed", e.to_string()))?.flatten() {
        let n = e.file_name().to_string_lossy().to_string();
        let p = e.path();
        if n == "releases.json" || n.ends_with(".part") { let _ = fs::remove_file(&p); }
        else if lordos_imager_core::valid_tag(&n) && p.is_dir() {
            for f in fs::read_dir(&p).into_iter().flatten().flatten() { if f.path().is_file() { let _ = fs::remove_file(f.path()); } }
            let _ = fs::remove_dir(&p);
        }
    }
    Ok(())
}

fn main() {
    let config = config_dir();
    let opts = read_json(&config.join("options.json"));
    let cache = opts.get("cacheDir").and_then(|v| v.as_str()).map(PathBuf::from).unwrap_or_else(default_cache);
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(App { cancel: Arc::new(AtomicBool::new(false)), cache: Mutex::new(cache), config })
        .invoke_handler(tauri::generate_handler![
            platform, list_drives, fetch_releases, cached_releases, cache_state, download, cancel, read_cache_text, check_sums,
            image_info, read_key_file, hash_password, wifi_psk, write_image, write_firstrun, eject,
            load_options, save_options, load_profiles, save_profiles, cache_size, clear_cache
        ])
        .setup(|app| {
            if let Some(w) = app.get_webview_window("main") { let _ = w.set_title("LordOS Imager"); }
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("LordOS Imager");
}
