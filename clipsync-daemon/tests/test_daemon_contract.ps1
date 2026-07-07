Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$service   = Get-Content -Raw (Join-Path $root "module/service.sh")
$daemon    = Get-Content -Raw (Join-Path $root "clipsyncd.c")
$dconf     = Get-Content -Raw (Join-Path $root "daemon_config.h")
$makefile  = Get-Content -Raw (Join-Path $root "Makefile")
$zygisk    = Get-Content -Raw (Join-Path $root "zygisk/jni/main.cpp")
$bridge    = Get-Content -Raw (Join-Path $root "bridge_protocol.c")
$bclient   = Get-Content -Raw (Join-Path $root "clip_bridge_client.c")
$ws        = Get-Content -Raw (Join-Path $root "ws_server.c")
$last      = Get-Content -Raw (Join-Path $root "last_clip.c")
$lastTest  = Get-Content -Raw (Join-Path $root "test_last_clip.c")
$mdns      = Get-Content -Raw (Join-Path $root "mdns_publish.c")

# ── Service / config path ─────────────────────────────────────────

if ($service -notmatch "system/bin/clipsyncd") {
    throw "service.sh must launch the packaged daemon"
}
if ($service -match "/data/adb/modules/clipsyncd/clipsyncd") {
    throw "service.sh must not reference obsolete module-root daemon path"
}
if ($service -notmatch "--config") {
    throw "service.sh must pass clipsync.toml path to clipsyncd"
}
if ($dconf -notmatch "/data/adb/modules/clipsyncd/config/clipsync\.toml") {
    throw "daemon config default path must point to packaged clipsync.toml"
}
if ($daemon -notmatch "clipsync_config_load_from_args") {
    throw "clipsyncd must load config before starting services"
}

# ── Security ──────────────────────────────────────────────────────

if ($zygisk -notmatch "SO_PEERCRED" -or $zygisk -notmatch "cred\.uid != 0") {
    throw "Zygisk bridge must restrict @clipbridge clients to root (SO_PEERCRED)"
}
if ($zygisk -notmatch '"android"') {
    throw "Zygisk bridge must use a UID-1000-owned package name"
}
if ($zygisk -match "com\.android\.shell") {
    throw "Zygisk bridge runs as system_server, must not claim com.android.shell"
}
if ($zygisk -notmatch "preAppSpecialize" -or $zygisk -notmatch "DLCLOSE_MODULE_LIBRARY") {
    throw "Zygisk must request module dlclose in ordinary app processes"
}

# ── Clipboard bridge architecture ─────────────────────────────────

if ($daemon -notmatch "clip_bridge_init" -or
    $daemon -notmatch "clip_bridge_get_text" -or
    $daemon -notmatch "clip_bridge_set_text") {
    throw "clipsyncd must use clip_bridge_* for clipboard access"
}
if ($makefile -match "(?m)^LDFLAGS\s*=.*-lbinder_ndk") {
    throw "main daemon must not link libbinder_ndk"
}
if ($makefile -match "binder_clip") {
    throw "build must not reference legacy binder_clip symbols"
}

# ── Event-driven WATCH (no polling) ───────────────────────────────

if ($daemon -notmatch "clip_bridge_watch_start" -or
    $daemon -notmatch "clip_bridge_watch_take_changed") {
    throw "clipsyncd must use event-driven clipbridge WATCH"
}
if ($daemon -match "clip_bridge_watch_is_ready" -or
    $daemon -match "clipboard_poll_ticks" -or
    $makefile -match "clipboard_poll\.c") {
    throw "no periodic clipboard polling allowed"
}
if ($daemon -notmatch "clip_bridge_watch_start\s*\(\s*wake_main_loop" -or
    $bclient -notmatch "g_watch_notify_fn") {
    throw "WATCH thread must wake daemon event loop on clipboard change"
}

# ── Bridge protocol ───────────────────────────────────────────────

if ($zygisk -notmatch "WRITE <len>" -or $zygisk -notmatch "DATA %lu") {
    throw "Zygisk bridge must use length-prefixed protocol"
}
if ($bclient -notmatch "WATCH\\n" -or $bclient -notmatch "CLIPSYNC_WATCH_LINE_CHANGED") {
    throw "clip_bridge_client must implement WATCH notifications"
}
if ($bridge -notmatch "READY\\n" -or $bridge -notmatch "CHANGED\\n") {
    throw "bridge protocol must handle WATCH READY/CHANGED lines"
}

# ── InMemoryDexClassLoader ────────────────────────────────────────

if ($zygisk -notmatch "InMemoryDexClassLoader") {
    throw "Zygisk must load helper via InMemoryDexClassLoader"
}
if ($zygisk -notmatch "REGISTER_ZYGISK_COMPANION") {
    throw "Zygisk must have a companion handler for DEX transfer"
}
if ($makefile -notmatch "clipsync-helper\.dex" -or $makefile -notmatch "d8") {
    throw "make module must build and package helper.dex"
}

# ── WebSocket / mDNS ──────────────────────────────────────────────

if ($ws -notmatch "mg_http_listen" -or $ws -notmatch "mg_mgr_poll") {
    throw "ws_server must use real Mongoose HTTP listener + event loop"
}
if ($ws -notmatch "mg_ws_upgrade" -or $ws -notmatch "mg_ws_send") {
    throw "ws_server must upgrade and send real WebSocket frames"
}
if ($ws -notmatch "AUTH_TIMEOUT_MS" -or $ws -notmatch "auth_deadline_ms") {
    throw "ws_server must close unauthenticated clients after auth deadline"
}
if ($mdns -notmatch "mg_mdns_listen") {
    throw "mdns_publish must register a real Mongoose mDNS listener"
}
if ($mdns -notmatch "_clipsync\._tcp") {
    throw "mdns must advertise _clipsync._tcp service type"
}

# ── Clipboard deduplication ───────────────────────────────────────

if ($last -notmatch "FNV1A64" -or $last -notmatch "memcmp") {
    throw "clipboard dedupe must use hash + full content compare"
}
if ($lastTest -notmatch "70000") {
    throw "dedupe tests must cover clipboard content larger than 64KB"
}

# ── Build / packaging ─────────────────────────────────────────────

if ($makefile -notmatch "module\\config\\clipsync\.toml") {
    throw "make module must package clipsync.toml"
}
if ($makefile -match "(?m)^\s*rm\s" -or $makefile -match "\|\|\s*true") {
    throw "Makefile must not depend on Unix rm or shell '|| true'"
}
if ($makefile -notmatch "ReadAllBytes\('\$\(ZYGISK_BUILD_SO\)'\)") {
    throw "make module must verify Zygisk .so copy integrity"
}

Write-Host "daemon contract checks passed ($((Get-Variable).Count) checks)"
