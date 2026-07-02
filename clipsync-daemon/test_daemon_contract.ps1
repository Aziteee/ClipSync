Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$service = Get-Content -Raw (Join-Path $root "module/service.sh")
$wsServer = Get-Content -Raw (Join-Path $root "ws_server.c")

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

Write-Host "daemon contract checks passed"
