Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$service = Get-Content -Raw (Join-Path $root "module/service.sh")
$sepolicyRule = Get-Content -Raw (Join-Path $root "module/sepolicy.rule")
$daemonMain = Get-Content -Raw (Join-Path $root "clipsyncd.c")
$daemonConfig = Get-Content -Raw (Join-Path $root "daemon_config.h")
$makefile = Get-Content -Raw (Join-Path $root "Makefile")
$zygiskMain = Get-Content -Raw (Join-Path $root "zygisk/jni/main.cpp")
$bridgeProtocol = Get-Content -Raw (Join-Path $root "bridge_protocol.c")
$clipBridgeClient = Get-Content -Raw (Join-Path $root "clip_bridge_client.c")
$wsServer = Get-Content -Raw (Join-Path $root "ws_server.c")
$lastClip = Get-Content -Raw (Join-Path $root "last_clip.c")
$lastClipTest = Get-Content -Raw (Join-Path $root "test_last_clip.c")
$mdnsPublish = Get-Content -Raw (Join-Path $root "mdns_publish.c")
$legacyClipPrefix = "binder" + "_clip"
$legacyClipSourcePattern = $legacyClipPrefix + "\.c"
$legacyClipSymbolPattern = $legacyClipPrefix + "_"

if ($service -notmatch "system/bin/clipsyncd") {
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

if ($makefile -notmatch "module[\\/]system[\\/]etc[\\/]clipsync-helper\.jar" -or $makefile -notmatch "d8") {
    throw "make module must build and package the ClipSync helper dex jar"
}

if (Test-Path (Join-Path $root "module/post-fs-data.sh")) {
    throw "module must not copy helper jar into /data/system; use systemless module/system/etc instead"
}

if (Test-Path (Join-Path $root "module/skip_mount")) {
    throw "module must not disable systemless mounting because helper jar is exposed via module/system/etc"
}

if ($sepolicyRule -notmatch "allow\s+system_server\s+adb_data_file\s+file\s+\{[^}]*getattr[^}]*open[^}]*read[^}]*map[^}]*\}") {
    throw "module sepolicy must let system_server read the mounted helper jar when module files keep adb_data_file labels"
}

if ($makefile -notmatch "clip_bridge_client\.c") {
    throw "daemon sources must use clip_bridge_client.c for the clipbridge socket client"
}

if ($makefile -notmatch "bridge_protocol\.c") {
    throw "daemon and zygisk builds must include the shared bridge protocol helper"
}

if ($makefile -notmatch "last_clip\.c" -or $makefile -notmatch "test_last_clip") {
    throw "daemon build must include exact last-clipboard dedupe helper and tests"
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

if ($daemonMain -notmatch "handle_clipboard_event") {
    throw "clipsyncd must handle clipbridge WATCH events for phone-to-PC sync"
}

if ($daemonMain -notmatch "clip_bridge_watch_start" -or $daemonMain -notmatch "clip_bridge_watch_take_changed") {
    throw "clipsyncd must use event-driven clipbridge WATCH notifications"
}

if ($daemonMain -notmatch "clip_bridge_watch_start\s*\(\s*wake_main_loop" -or
    $clipBridgeClient -notmatch "g_watch_notify_fn" -or
    $clipBridgeClient -notmatch "g_watch_notify_fn\s*\(\s*g_watch_notify_arg\s*\)") {
    throw "WATCH thread must wake the daemon event loop when clipboard changes arrive"
}

if ($daemonMain -match "ws_server_poll\s*\(\s*50\s*\)" -or
    $daemonMain -match "status_update_ticks" -or
    $daemonMain -match "mdns_announce_ticks") {
    throw "clipsyncd must not use the old fixed 50ms tick loop"
}

if ($daemonMain -notmatch "MAX_IDLE_POLL_MS\s+5000" -or
    $daemonMain -notmatch "CLOCK_MONOTONIC" -or
    $daemonMain -notmatch "ws_server_next_timeout_ms") {
    throw "clipsyncd must use dynamic long poll timeouts with monotonic mDNS deadlines"
}

if ($clipBridgeClient -notmatch "WATCH\\n" -or $clipBridgeClient -notmatch "CLIPSYNC_WATCH_LINE_CHANGED") {
    throw "clip_bridge_client must implement WATCH change notifications"
}

if ($bridgeProtocol -notmatch "READY\\n" -or $bridgeProtocol -notmatch "CHANGED\\n") {
    throw "bridge protocol must parse WATCH READY/CHANGED lines"
}

if ($daemonMain -match "clip_bridge_watch_is_ready" -or $daemonMain -match "clipboard_poll_ticks" -or $daemonMain -match "clipsync_clipboard_poll_ticks_for_clients") {
    throw "clipsyncd must not fall back to periodic clipboard polling"
}

if ($daemonMain -match "polling fallback" -or $daemonMain -match "using polling") {
    throw "clipsyncd must not advertise or use polling fallback"
}

if ($makefile -match "clipboard_poll\.c" -or $makefile -match "test_clipboard_poll") {
    throw "daemon build must not include clipboard polling helpers"
}

if ($wsServer -notmatch "AUTH_TIMEOUT_MS" -or $wsServer -notmatch "auth_deadline_ms") {
    throw "WebSocket server must close unauthenticated clients after an auth deadline"
}

if ($wsServer -notmatch "mg_wakeup_init" -or
    $wsServer -notmatch "mg_wakeup" -or
    $wsServer -notmatch "MG_EV_WAKEUP" -or
    $wsServer -notmatch "ws_server_next_timeout_ms") {
    throw "WebSocket server must support wakeup-driven long polling"
}

if ($daemonMain -match "g_last_text" -or $daemonMain -match "65536") {
    throw "daemon must not use fixed 64KB clipboard dedupe storage"
}

if ($lastClip -notmatch "FNV1A64" -or
    $lastClip -notmatch "memcmp" -or
    $lastClipTest -notmatch "70000" -or
    $lastClipTest -notmatch "hash collision") {
    throw "last clipboard dedupe must use length/hash plus byte compare and cover large text"
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

if ($zygiskMain -notmatch "preAppSpecialize" -or $zygiskMain -notmatch "DLCLOSE_MODULE_LIBRARY") {
    throw "Zygisk bridge must request module dlclose in ordinary app processes"
}

if ($zygiskMain -notmatch "SO_PEERCRED" -or $zygiskMain -notmatch "cred\.uid != 0") {
    throw "Zygisk bridge must restrict @clipbridge clients by peer credentials"
}

if ($zygiskMain -notmatch "WRITE <len>" -or $zygiskMain -notmatch "DATA %lu") {
    throw "Zygisk bridge must use the length-prefixed bridge protocol"
}

if ($zygiskMain -notmatch "addPrimaryClipChangedListener" -or $zygiskMain -notmatch "IOnPrimaryClipChangedListener") {
    throw "Zygisk bridge must register an IOnPrimaryClipChangedListener"
}

if ($zygiskMain -notmatch "InMemoryDexClassLoader") {
    throw "Zygisk bridge must load the helper DEX via InMemoryDexClassLoader from companion process"
}

if ($zygiskMain -notmatch "REGISTER_ZYGISK_COMPANION") {
    throw "Zygisk bridge must register a companion handler for DEX transfer"
}
}

if ($zygiskMain -match "/data/system/clipsync-helper\.jar") {
    throw "Zygisk bridge must not depend on a copied /data/system helper jar"
}

if ($zygiskMain -notmatch "WATCH\\n" -or $zygiskMain -notmatch "CHANGED\\n" -or $zygiskMain -notmatch "MSG_DONTWAIT") {
    throw "Zygisk bridge must expose event-driven WATCH notifications"
}

if ($zygiskMain -notmatch "watch_unavailable" -or $zygiskMain -notmatch "g_clip_listener") {
    throw "Zygisk bridge must reject WATCH when listener registration failed"
}

Write-Host "daemon contract checks passed"
