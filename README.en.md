# ClipSync

ClipSync is a local-network clipboard sync project that synchronizes the text clipboard between a Windows PC and Android devices. The design goal is to keep it lightweight, easy to use, and low-power.

## Features

- Two-way text clipboard sync between PC and multiple Android devices
- mDNS auto-discovery + active LAN scanning for seamless connection
- Windows tray status indicator
- Pre-shared key authentication

## Quick Start

Download from the [Release](https://github.com/Aziteee/ClipSync/releases) page:

- **PC**: `clipsync-pc.exe`
- **Android**: `clipsyncd-module.zip`

### Android

1. Make sure Zygisk is installed
2. Flash the `clipsyncd-module.zip` module in your root manager
3. Reboot your phone

### PC

Double-click `clipsync-pc.exe`; a tray icon appears and syncing starts.

> Make sure the PC and phone are on the same local network.

## Configuration

The default configuration works **out of the box** with no setup required. The options below only need to be changed for customization.

> The `port` and `secret` on the PC and Android sides must match, otherwise they cannot connect.

### Android Side

Config file: `/data/adb/clipsyncd/clipsync.toml`

```toml
[connection]
port = 5287                          # WebSocket port
mdns_announce_interval_ms = 30000    # mDNS announce interval (ms, range 1000-3600000)

[auth]
secret = ""                          # Pre-shared key; empty means no verification
```

After editing, click the module's Action button in your root manager to restart the service. No reboot needed.

### PC Side

Config file: `clipsync.toml` in the same directory as `clipsync-pc.exe`

```toml
[connection]
port = 5287                          # WebSocket port (must match Android side)
heartbeat_interval_ms = 10000        # Heartbeat send interval (ms)
heartbeat_timeout_ms = 30000         # Heartbeat timeout (ms)
lan_scan_interval = 0                # LAN scan: 0=once at startup, -1=disabled, N=every N seconds

[auth]
secret = ""                          # Pre-shared key (must match Android side)

[clipboard]
debounce_ms = 300                    # Clipboard change debounce interval (ms)

[general]
start_with_windows = false           # Launch at Windows startup

# Manually specify device list (optional). When set, mDNS and LAN scan are skipped;
# only devices with enabled = true in the list are connected
# [[devices]]
# name = "Phone"
# uri = "ws://192.168.0.10:5287/ws"
# enabled = true
#
# [[devices]]
# name = "Tablet"
# uri = "ws://192.168.0.11:5287/ws"
# enabled = false
```

Restart `clipsync-pc.exe` for changes to take effect.

### Configuration Reference

**Common**

| Field | Default | Description |
|-------|---------|-------------|
| `connection.port` | `5287` | WebSocket port; must match on both sides |
| `auth.secret` | `""` | Pre-shared key; must match on both sides; empty means no verification |
| `clipboard.debounce_ms` | `300` | Clipboard change debounce interval (ms) |

**Android only**

| Field | Default | Description |
|-------|---------|-------------|
| `connection.mdns_announce_interval_ms` | `30000` | mDNS announce interval (ms, range 1000-3600000) |

**PC only**

| Field | Default | Description |
|-------|---------|-------------|
| `connection.heartbeat_interval_ms` | `10000` | WS heartbeat send interval (ms) |
| `connection.heartbeat_timeout_ms` | `30000` | WS heartbeat timeout (ms) |
| `connection.lan_scan_interval` | `0` | LAN scan: `0`=once at startup, `-1`=disabled, positive=every N seconds |
| `general.start_with_windows` | `false` | Launch at Windows startup |
| `devices[].name` | none | Manual device display name (optional) |
| `devices[].uri` | none | Manual device WebSocket address |
| `devices[].enabled` | `true` | Whether this device is enabled |

## Development

Project structure:

- `clipsync-pc/`: Windows tray client; listens to / writes the PC clipboard and connects to the Android WebSocket server.
- `clipsync-daemon/`: Android KernelSU/Zygisk module and the `clipsyncd` daemon; accesses the Android clipboard via the Zygisk bridge and provides sync services.

Requirements:

- Windows + PowerShell
- Rust toolchain
- Android NDK
- A rooted Android device with KernelSU + Zygisk enabled
- `adb` available

PC side:

```powershell
cd clipsync-pc
cargo build --release
```

Android side:

```powershell
cd clipsync-daemon
$env:ANDROID_NDK_ROOT = "path-to-your-ndk"
$env:CC = "aarch64-linux-android33-clang"
make module
```

Package the KernelSU module zip:

```powershell
make package
```

## Notes

- Theoretically supports Windows 10/11 and Android 10+.
