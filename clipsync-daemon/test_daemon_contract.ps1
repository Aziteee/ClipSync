Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$service = Get-Content -Raw (Join-Path $root "module/service.sh")
$daemonMain = Get-Content -Raw (Join-Path $root "clipsyncd.c")
$zygiskMain = Get-Content -Raw (Join-Path $root "zygisk/jni/main.cpp")
$wsServer = Get-Content -Raw (Join-Path $root "ws_server.c")
$mdnsPublish = Get-Content -Raw (Join-Path $root "mdns_publish.c")

if ($service -notmatch "/data/adb/modules/clipsyncd/system/bin/clipsyncd") {
    throw "service.sh must launch the packaged daemon at system/bin/clipsyncd"
}

if ($service -match "/data/adb/modules/clipsyncd/clipsyncd") {
    throw "service.sh still references the obsolete module-root daemon path"
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

if ($zygiskMain -match "com\.android\.shell") {
    throw "Zygisk clipboard bridge runs as system_server and must not claim com.android.shell"
}

if ($zygiskMain -notmatch '"android"') {
    throw "Zygisk clipboard bridge must use a package name owned by UID 1000"
}

Write-Host "daemon contract checks passed"
