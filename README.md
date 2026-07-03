# ClipSync

ClipSync 是一个局域网剪贴板同步项目，用于在 Windows PC 和 Android 设备之间同步文本剪贴板。

## 项目结构

- `clipsync-pc/`：Windows 托盘客户端，负责监听/写入 PC 剪贴板，并连接 Android 端 WebSocket 服务。
- `clipsync-daemon/`：Android 端 KernelSU/Zygisk 模块和 `clipsyncd` 守护进程，负责访问 Android 剪贴板并提供同步服务。

## 功能

- PC 与 Android 双向同步文本剪贴板
- WebSocket 通信，默认端口 `5287`
- 预共享密钥认证
- mDNS 自动发现，无感连接
- Windows 托盘状态显示

## 使用指南

从项目的 Release 页面下载：

- Windows 客户端：`clipsync-pc.exe`
- Android 模块：`clipsyncd-module.zip`

### Android 端

确保你已有 `Zygisk` 环境，然后在 KernelSU 管理器中安装 `clipsyncd-module.zip`，重启手机。

Android 模块会读取：

```text
/data/adb/modules/clipsyncd/config/clipsync.toml
```

默认配置如下：

```toml
[connection]
port = 5287

[auth]
secret = ""

[clipboard]
debounce_ms = 300
```

如果需要修改端口或认证密钥，编辑该文件后重启手机，或手动重启 `clipsyncd`。

### PC 端

将 `clipsync-pc.exe` 放到一个单独目录中，双击打开即可。

如果需要修改端口或认证密钥，请在同目录创建 `clipsync.toml`：

```toml
[connection]
port = 5287
# 如果自动发现不可用，取消下面一行注释并填写手机 IP：
# host = "192.168.0.103"

[auth]
secret = ""

[clipboard]
debounce_ms = 300
```

之后重新运行 `clipsync-pc.exe` 即可。

PC 端的 `port` 和 `secret` 需要与 Android 端保持一致。

## 开发

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

- 仅在 Windows 11 及 ColorOS16 环境测试成功，其他环境不保证正常运行。
