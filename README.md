# ClipSync

ClipSync 是一个局域网剪贴板同步项目，用于在 Windows PC 和 Android 设备之间同步文本剪贴板。

## 功能

- PC 与 Android 双向同步文本剪贴板
- 预共享密钥认证
- mDNS 自动发现，无感连接
- Windows 托盘状态显示
- 轻量，对性能无影响

## 快速开始

从 [Release](https://github.com/Aziteee/ClipSync/releases) 页面下载：

- **PC**：`clipsync-pc.exe`
- **Android**：`clipsyncd-module.zip`

### Android

1. 确保已安装 Zygisk
2. 在 KernelSU 管理器中刷入 `clipsyncd-module.zip`
3. 重启手机

### PC

双击 `clipsync-pc.exe`，任务栏出现图标即开始同步。

> 确保 PC 和手机在同一局域网。

## 配置

以下为默认配置，**开箱即用**。如需自定义，可创建配置文件。

### Android

配置文件路径：`/data/adb/modules/clipsyncd/config/clipsync.toml`

```toml
[connection]
port = 5287

[auth]
secret = ""

[clipboard]
debounce_ms = 300
```

修改后重启手机，或手动重启 `clipsyncd`。

### PC

配置文件路径：与 `clipsync-pc.exe` 同目录的 `clipsync.toml`

```toml
[connection]
port = 5287
# 若 mDNS 自动发现不可用，填写手机 IP：
# host = "192.168.0.103"

[auth]
secret = ""

[clipboard]
debounce_ms = 300
```

修改后重启 `clipsync-pc.exe`。

> PC 与 Android 的 `port` 和 `secret` 必须一致。

### 配置项说明

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `connection.port` | `5287` | WebSocket 端口 |
| `connection.host` | 自动发现 | 手动指定手机 IP（PC 端） |
| `connection.uri` | 自动发现 | 手动指定 WebSocket URI（PC 端） |
| `connection.heartbeat_interval_ms` | `5000` | 心跳发送间隔（毫秒，PC 端） |
| `connection.heartbeat_timeout_ms` | `15000` | 心跳超时时间（毫秒，PC 端） |
| `auth.secret` | `""` | 预共享密钥，为空则不校验 |
| `clipboard.debounce_ms` | `300` | 去抖间隔（毫秒） |

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

- 仅在 Windows 11 及 Android 16 (ColorOS) 环境测试成功，其他环境不保证正常运行。
