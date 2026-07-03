Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$service = Get-Content -Raw (Join-Path $root "module/service.sh")
$daemonMain = Get-Content -Raw (Join-Path $root "clipsyncd.c")
$daemonConfig = Get-Content -Raw (Join-Path $root "daemon_config.h")
$makefile = Get-Content -Raw (Join-Path $root "Makefile")
$zygiskMain = Get-Content -Raw (Join-Path $root "zygisk/jni/main.cpp")
$wsServer = Get-Content -Raw (Join-Path $root "ws_server.c")
$mdnsPublish = Get-Content -Raw (Join-Path $root "mdns_publish.c")
$legacyClipPrefix = "binder" + "_clip"
$legacyClipSourcePattern = $legacyClipPrefix + "\.c"
$legacyClipSymbolPattern = $legacyClipPrefix + "_"

if ($service -notmatch "/data/adb/modules/clipsyncd/system/bin/clipsyncd") {
    throw "service.sh must launch the packaged daemon at system/bin/clipsyncd"
}

if ($service -match "/data/adb/modules/clipsyncd/clipsyncd") {
    throw "service.sh still references the obsolete module-root daemon path"
}

if ($service -notmatch "--config") {
    throw "service.sh must pass the packaged clipsync.toml path to clipsyncd"
}

if ($daemonConfig -notmatch "/data/adb/modules/clipsyncd/config/clipsync\.toml") {
    throw "daemon must default to the packaged clipsync.toml path"
}

if ($daemonMain -notmatch "clipsync_config_load_from_args") {
    throw "clipsyncd must load clipsync.toml before starting services"
}

if ($makefile -notmatch "module\\config\\clipsync\.toml") {
    throw "make module must package clipsync.toml into module/config"
}

if ($makefile -notmatch "clip_bridge_client\.c") {
    throw "daemon sources must use clip_bridge_client.c for the clipbridge socket client"
}

if ($makefile -notmatch "bridge_protocol\.c") {
    throw "daemon and zygisk builds must include the shared bridge protocol helper"
}

if ($makefile -match "SRCS\s*=.*$legacyClipSourcePattern") {
    throw "daemon sources must not use the legacy Binder-named clipbridge client source"
}

if ($makefile -match "(?m)^LDFLAGS\s*=.*-lbinder_ndk") {
    throw "main daemon LDFLAGS must not link libbinder_ndk"
}

if ($makefile -notmatch "(?m)^TEST_CLIP_LDFLAGS\s*=.*-lbinder_ndk") {
    throw "test_clip diagnostic must link libbinder_ndk via TEST_CLIP_LDFLAGS"
}

if ($daemonMain -match $legacyClipSymbolPattern) {
    throw "clipsyncd main path must use clip_bridge_* symbols, not the legacy Binder-named symbols"
}

if ($daemonMain -notmatch "clip_bridge_init" -or $daemonMain -notmatch "clip_bridge_get_text" -or $daemonMain -notmatch "clip_bridge_set_text") {
    throw "clipsyncd must call clip_bridge_init/get_text/set_text"
}

if ($wsServer -notmatch "mg_http_listen") {
    throw "ws_server_init must create a real Mongoose HTTP listener"
}

if ($wsServer -notmatch "mg_mgr_poll") {
    throw "ws_server_poll must drive the Mongoose event manager"
}

if ($wsServer -notmatch "mg_ws_upgrade") {
    throw "WebSocket server must upgrade HTTP connections"
}

if ($wsServer -notmatch "mg_ws_send") {
    throw "WebSocket server must send real WebSocket frames"
}

if ($mdnsPublish -notmatch "mg_mdns_listen") {
    throw "mdns_publish must register a real Mongoose mDNS listener"
}

if ($mdnsPublish -notmatch "_clipsync\._tcp") {
    throw "mdns_publish must advertise _clipsync._tcp"
}

if ($mdnsPublish -notmatch '"\\x11"\s+"auth=hmac-sha256"') {
    throw "mdns DNS-SD TXT length for auth=hmac-sha256 must be 0x11"
}

if ($daemonMain -notmatch "poll_clipboard_change") {
    throw "clipsyncd must poll Android clipboard changes for phone-to-PC sync"
}

if ($daemonMain -notmatch "ws_server_authenticated_count") {
    throw "clipsyncd must adapt clipboard polling based on authenticated WebSocket clients"
}

if ($daemonMain -notmatch "cfg\.debounce_ms") {
    throw "clipsyncd must apply configured clipboard.debounce_ms to polling"
}

if ($wsServer -notmatch "AUTH_TIMEOUT_MS" -or $wsServer -notmatch "auth_deadline_ms") {
    throw "WebSocket server must close unauthenticated clients after an auth deadline"
}

if ($wsServer -notmatch "#define MAX_CLIENTS 1") {
    throw "WebSocket server must allow only one PC connection"
}

if ($daemonMain -notmatch "PC: %s" -or $daemonMain -notmatch "Connected" -or $daemonMain -notmatch "Waiting") {
    throw "module description must include PC connection state"
}

if ($makefile -notmatch "ReadAllBytes\('\$\(ZYGISK_BUILD_SO\)'\)" -or $makefile -notmatch "Zygisk module copy verification failed") {
    throw "make module must verify and repair the packaged Zygisk .so copy"
}

if ($makefile -match "(?m)^\s*rm\s" -or $makefile -match "\|\|\s*true") {
    throw "Makefile clean/module rules must not depend on Unix rm or shell '|| true'"
}

if ($zygiskMain -match "com\.android\.shell") {
    throw "Zygisk clipboard bridge runs as system_server and must not claim com.android.shell"
}

if ($zygiskMain -notmatch '"android"') {
    throw "Zygisk clipboard bridge must use a package name owned by UID 1000"
}

if ($zygiskMain -notmatch "SO_PEERCRED" -or $zygiskMain -notmatch "cred\.uid != 0") {
    throw "Zygisk bridge must restrict @clipbridge clients by peer credentials"
}

if ($zygiskMain -notmatch "WRITE <len>" -or $zygiskMain -notmatch "DATA %lu") {
    throw "Zygisk bridge must use the length-prefixed bridge protocol"
}

Write-Host "daemon contract checks passed"
