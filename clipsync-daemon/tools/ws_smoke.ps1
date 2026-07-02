param(
    [Parameter(Mandatory=$true)]
    [string]$Uri,

    [string]$Secret = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function ConvertTo-HexLower([byte[]]$Bytes) {
    ($Bytes | ForEach-Object { $_.ToString("x2") }) -join ""
}

$ws = [System.Net.WebSockets.ClientWebSocket]::new()
$ct = [Threading.CancellationToken]::None
$null = $ws.ConnectAsync([Uri]$Uri, $ct).GetAwaiter().GetResult()

$buffer = New-Object byte[] 4096
$segment = [ArraySegment[byte]]::new($buffer)
$result = $ws.ReceiveAsync($segment, $ct).GetAwaiter().GetResult()
$helloJson = [Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count)
$hello = $helloJson | ConvertFrom-Json

if ($hello.type -ne "hello") {
    throw "Expected hello, got: $helloJson"
}

$hmac = [System.Security.Cryptography.HMACSHA256]::new([Text.Encoding]::UTF8.GetBytes($Secret))
$digest = $hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes([string]$hello.challenge))
$response = ConvertTo-HexLower $digest
$authJson = @{ type = "auth"; response = $response } | ConvertTo-Json -Compress
$authBytes = [Text.Encoding]::UTF8.GetBytes($authJson)
$null = $ws.SendAsync([ArraySegment[byte]]::new($authBytes), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).GetAwaiter().GetResult()

$result = $ws.ReceiveAsync($segment, $ct).GetAwaiter().GetResult()
$authResultJson = [Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count)
$authResult = $authResultJson | ConvertFrom-Json

if ($authResult.type -ne "auth_ok") {
    throw "Expected auth_ok, got: $authResultJson"
}

$pingJson = @{ type = "ping" } | ConvertTo-Json -Compress
$pingBytes = [Text.Encoding]::UTF8.GetBytes($pingJson)
$null = $ws.SendAsync([ArraySegment[byte]]::new($pingBytes), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).GetAwaiter().GetResult()

$result = $ws.ReceiveAsync($segment, $ct).GetAwaiter().GetResult()
$pongJson = [Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count)
$pong = $pongJson | ConvertFrom-Json

if ($pong.type -ne "pong") {
    throw "Expected pong, got: $pongJson"
}

$null = $ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, "done", $ct).GetAwaiter().GetResult()
Write-Host "websocket smoke passed: $Uri"
