# ClipSync

[English](README.en.md)

ClipSync 是一个局域网剪贴板同步项目，用于在 Windows PC 和 Android 设备之间同步文本剪贴板。设计理念是尽量做到轻量、易用、低能耗。

## 功能

- PC 与多台 Android 设备双向同步文本剪贴板
- mDNS 自动发现 + LAN 主动扫描，无感连接
- Windows 托盘状态显示
- 预共享密钥认证

## 快速开始

从 [Release](https://github.com/Aziteee/ClipSync/releases) 页面下载：

- **PC**：`clipsync-pc.exe`
- **Android**：`clipsyncd-module.zip`

### Android

1. 确保已安装 Zygisk
2. 在 Root 管理器中刷入 `clipsyncd-module.zip` 模块
3. 重启手机

### PC

双击 `clipsync-pc.exe`，任务栏出现图标即开始同步。

> 确保 PC 和手机在同一局域网。

## 配置

默认配置**开箱即用**，无需任何配置即可使用。以下配置项仅在需要自定义时修改。

> PC 与 Android 的 `port` 和 `secret` 必须一致，否则无法连接。

### Android 端

配置文件：`/data/adb/clipsyncd/clipsync.toml`

```toml
[connection]
port = 5287                          # WebSocket 端口
mdns_announce_interval_ms = 30000    # mDNS 通告间隔（毫秒，范围 1000-3600000）

[auth]
secret = ""                          # 预共享密钥，为空则不校验
```

修改后在 Root 管理器中点击模块的 Action 按钮重启服务即可生效，无需重启手机。

### PC 端

配置文件：与 `clipsync-pc.exe` 同目录的 `clipsync.toml`

```toml
[connection]
port = 5287                          # WebSocket 端口（须与 Android 端一致）
heartbeat_interval_ms = 10000        # 心跳发送间隔（毫秒）
heartbeat_timeout_ms = 30000         # 心跳超时时间（毫秒）
lan_scan_interval = 0                # LAN 扫描：0=仅启动时一次，-1=关闭，N=每 N 秒周期扫描

[auth]
secret = ""                          # 预共享密钥（须与 Android 端一致）

[clipboard]
debounce_ms = 300                    # 剪贴板变化去抖间隔（毫秒）

[general]
start_with_windows = false           # 开机自启

# 手动指定设备列表（可选）。配置后跳过 mDNS 与 LAN 扫描，仅连接列表中 enabled = true 的设备
# [[devices]]
# name = "手机"
# uri = "ws://192.168.0.10:5287/ws"
# enabled = true
#
# [[devices]]
# name = "平板"
# uri = "ws://192.168.0.11:5287/ws"
# enabled = false
```

修改后重启 `clipsync-pc.exe` 生效。

### 配置项参考

**通用**

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `connection.port` | `5287` | WebSocket 端口，两端必须一致 |
| `auth.secret` | `""` | 预共享密钥，两端必须一致；为空则不校验 |
| `clipboard.debounce_ms` | `300` | 剪贴板变化去抖间隔（毫秒） |

**仅 Android 端**

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `connection.mdns_announce_interval_ms` | `30000` | mDNS 通告间隔（毫秒，范围 1000-3600000） |

**仅 PC 端**

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `connection.heartbeat_interval_ms` | `10000` | WS心跳发送间隔（毫秒） |
| `connection.heartbeat_timeout_ms` | `30000` | WS心跳超时时间（毫秒） |
| `connection.lan_scan_interval` | `0` | LAN 扫描：`0`=仅启动时一次，`-1`=关闭，正数=每 N 秒周期扫描 |
| `general.start_with_windows` | `false` | 开机自启 |
| `devices[].name` | 无 | 手动设备显示名（可选） |
| `devices[].uri` | 无 | 手动设备 WebSocket 地址 |
| `devices[].enabled` | `true` | 是否启用该设备 |

## 开发

项目结构：

- `clipsync-pc/`：Windows 托盘客户端，负责监听/写入 PC 剪贴板，并连接 Android 端 WebSocket 服务。
- `clipsync-daemon/`：Android 端 KernelSU/Zygisk 模块和 `clipsyncd` 守护进程，负责通过 Zygisk bridge 访问 Android 剪贴板并提供同步服务。

环境要求：

- Windows + PowerShell
- Rust 工具链
- Android NDK
- 已 root 且启用 KernelSU + Zygisk 的 Android 设备
- `adb` 可用

PC 端：

```powershell
cd clipsync-pc
cargo build --release
```

Android 端：

```powershell
cd clipsync-daemon
$env:ANDROID_NDK_ROOT = "path-to-your-ndk"
$env:CC = "aarch64-linux-android33-clang"
make module
```

打包 KernelSU 模块 zip：

```powershell
make package
```

## 注意

- 理论支持 Windows 10/11 以及 Android 10+ 系统。
- 
