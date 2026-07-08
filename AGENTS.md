# ClipSync Agent 调试手册

## 项目架构

ClipSync 是一个局域网剪贴板同步项目，在 Windows PC 与多台 Android 设备之间双向同步文本剪贴板。整体由三个组件协作：PC 客户端（Rust）、Android 守护进程（C）、Zygisk 桥（C++ + Java helper，注入 `system_server`）。

### 组件总览

| 组件 | 目录 | 语言 | 运行位置 | 职责 |
|------|------|------|----------|------|
| `clipsync-pc` | `clipsync-pc/` | Rust | Windows | 托盘客户端；监听/写入 Windows 剪贴板；WebSocket 客户端连接各 Android 设备；mDNS 自动发现 + LAN 主动扫描 |
| `clipsyncd` | `clipsync-daemon/*.c` | C | Android (root) | 守护进程；WebSocket 服务端；mDNS 发布；通过 `@clipbridge` socket 读写 Android 剪贴板 |
| Zygisk bridge | `clipsync-daemon/zygisk/jni/main.cpp` | C++ | Android `system_server` 内 | 注入 `system_server`，通过 JNI 调用 `ClipboardService`，暴露 `@clipbridge` abstract Unix socket |
| Java helper | `clipsync-daemon/zygisk/helper/` | Java | `system_server` 内（dex 注入） | 注册 `IOnPrimaryClipChangedListener` 直接监听剪贴板变化，回调 native |

### 数据流

**Android → PC：**

```text
Android 剪贴板变化
  → ClipboardService 触发 IOnPrimaryClipChangedListener (Java helper)
  → nativeOnClipboardChanged() (Zygisk bridge, C++)
  → 通过 watcher fd 通知 clipsyncd
  → clipsyncd 调 READ 从 @clipbridge 取文本
  → 广播 clipboard_push JSON 给所有已认证 WS 客户端
  → PC 收到后写入 Windows 剪贴板 (Win32 SetClipboardData)
```

**PC → Android：**

```text
Windows 剪贴板变化 (WM_CLIPBOARDUPDATE)
  → clipsync-pc 去抖后发送 clipboard_set JSON
  → clipsyncd 收到 on_ws_set 回调
  → clipsyncd 发 WRITE <len>\n<body> 给 @clipbridge
  → Zygisk bridge 调 JNI 设置 ClipboardService 主剪贴板
```

### 剪贴板访问架构（核心链路）

`clipsyncd` 不直接使用 `libbinder_ndk`，主同步链路为：

```text
clipsyncd -> @clipbridge abstract Unix socket -> Zygisk bridge in system_server -> IClipboard/ClipboardService
```

- `IClipboard` 调用发生在注入 `system_server` 的 Zygisk bridge 内，绕过 Binder 权限检查。
- `clipsyncd` 只通过 `@clipbridge` 发送 `READ` / `WRITE` / `HAS` / `WATCH` 命令。
- Zygisk 生命周期关键点：socket 必须在 `postServerSpecialize` 启动（此时进程已是 `system_server`）；socket 线程需 `AttachCurrentThread`（`JNIEnv` 是线程局部的）。

### 通信协议

**WebSocket 消息**（JSON，`type` 字段区分，见 `protocol.h` / `clipsync-pc/src/protocol.rs`）：

| 消息 | 方向 | 说明 |
|------|------|------|
| `hello` | server→client | 握手，携带随机 `challenge` |
| `auth` | client→server | HMAC-SHA256(secret, challenge) 的 hex 响应 |
| `auth_ok` / `auth_fail` | server→client | 认证结果 |
| `clipboard_push` | server→client | Android 剪贴板变化推送 |
| `clipboard_set` | client→server | PC 请求设置 Android 剪贴板 |
| `ping` / `pong` | 双向 | 心跳 |

**Bridge 协议**（abstract Unix socket `@clipbridge`，长度前缀文本，见 `bridge_protocol.h`）：

- `READ\n` → 返回 `LEN <n>\n<body>`
- `WRITE <len>\n<body>` → 写入剪贴板
- `HAS\n` → 查询是否有剪贴板内容
- `WATCH\n` → 注册变化通知；bridge 通过 watcher fd 通知 `clipsyncd` 主循环

### 设备发现与连接

- **mDNS**：服务类型 `_clipsync._tcp`，端口 `5287`。Android 端 `mdns_publish.c` 通过 mongoose 发布；PC 端 `mdns.rs` 被动监听组播通告（`224.0.0.251:5353`）。
- **LAN scan**：PC 端 `lan_scan.rs` 主动扫描本机物理网口（Windows 通过 `GetIfTable2` 查询 ifType，仅选以太网/WiFi）的 `/24` 网段，对每个候选 IP:5287 尝试完整 WebSocket HMAC 握手，成功即认定为 ClipSync 设备。与 mDNS 共存互补，去重复用 URI 哈希。默认仅启动时扫一次（`lan_scan_interval = 0`），可配置周期扫描或关闭（`-1`）。托盘菜单"Scan LAN Now"支持手动触发。
- **手动配置**：`[[devices]]` 列表存在时跳过 mDNS 与 LAN scan，仅连接 `enabled = true` 的设备。
- PC 是 WebSocket **客户端**，Android `clipsyncd` 是 WebSocket **服务端**。

### 认证

预共享密钥 HMAC-SHA256 挑战-响应（`crypto_hmac.c` / `clipsync-pc/src/ws.rs`）。`secret` 为空时跳过校验。PC 与 Android 的 `port` 和 `secret` 必须一致。

### 模块结构

KernelSU/Zygisk 模块（`clipsync-daemon/module/`）：

```text
module/
├── module.prop          # 模块元信息 (id=clipsyncd)
├── service.sh           # 开机启动 clipsyncd（等待 sys.boot_completed）
├── action.sh            # KernelSU Action 按钮：重启 clipsyncd（改配置后免重启）
├── uninstall.sh         # 清理 /data/adb/clipsyncd 持久配置
├── config/clipsync.toml # 默认配置（首次启动复制到 /data/adb/clipsyncd/）
├── system/bin/clipsyncd # 守护进程二进制
└── zygisk/
    ├── arm64-v8a.so              # Zygisk 模块 .so (libzygisk_clipsync.so)
    └── clipsync-helper.dex       # Java helper dex (注册剪贴板监听)
```

- **持久配置路径**：`/data/adb/clipsyncd/clipsync.toml`（首次从模块默认配置复制）
- **Zygisk .so 路径**（运行时）：`/data/adb/modules/clipsyncd/zygisk/arm64-v8a.so`
- **状态展示**：`clipsyncd` 通过 `ksud module config set override.description` 把运行状态写入模块描述（Running/Bridge OK/PC Connected/Port）。

### clipsync-pc 模块

| 文件 | 职责 |
|------|------|
| `main.rs` | 入口；事件循环；设备连接管理；UI 事件分发 |
| `clip.rs` | Windows 剪贴板读写（Win32 API）；剪贴板变化监听（`WM_CLIPBOARDUPDATE`）；去抖 |
| `ws.rs` | WebSocket 连接与 HMAC 认证 |
| `protocol.rs` | `ClipMessage` 枚举与 JSON 序列化 |
| `sync.rs` | `SyncEngine`：blake3 去重；pending 队列 |
| `mdns.rs` | mDNS 服务发现（被动监听组播通告） |
| `lan_scan.rs` | LAN 主动扫描（`GetIfTable2` 选物理网口 + `/24` 探测 + 完整握手） |
| `tray.rs` | 系统托盘图标与菜单 |
| `config.rs` | TOML 配置解析 |
| `startup.rs` | 开机自启（注册表） |
| `update.rs` | 在线更新检查 |

### clipsyncd 模块（C）

| 文件 | 职责 |
|------|------|
| `clipsyncd.c` | main；事件循环（WS poll + bridge watch + mDNS 周期通告）；状态上报 |
| `clip_bridge_client.c` | `@clipbridge` socket 客户端；READ/WRITE/WATCH |
| `bridge_protocol.c` | bridge 长度前缀协议读写实现 |
| `ws_server.c` | mongoose WebSocket 服务端；认证；广播 |
| `mdns_publish.c` | mDNS 服务发布（mongoose）；启动时检测 SoftAP 接口并安装组播路由 |
| `protocol_json.c` | WebSocket JSON 消息构造/解析 |
| `crypto_hmac.c` | HMAC-SHA256 挑战-响应 |
| `daemon_config.c` | TOML 配置加载 |
| `last_clip.c` | 剪贴板去重（避免回环） |
| `softap_detect.c` | SoftAP 接口检测与组播路由安装（手机开热点时让 mDNS 走 `wlan2` 而非移动数据） |

### 关键约定

- **端口**：默认 `5287`
- **mDNS 服务类型**：`_clipsync._tcp`
- **Abstract socket 名**：`@clipbridge`（`sun_path[0]='\0'`，无文件系统路径、无 SELinux 标签问题）
- **日志 tag**：`ClipSyncBridge`（Android logcat）/ `clipsyncd`
- **架构**：仅 arm64-v8a；PC 仅 Windows
- **去重**：两端都基于内容哈希去重，防止同步回环

## 开发环境

- 平台：Windows + PowerShell
- 已连接 adb 设备（示例设备号 `293b43e2`）
- Android NDK 路径：`D:\AppData\Android\sdk\ndk\30.0.14904198`

## 一键编译

在 `clipsync-daemon/` 目录下执行：

```powershell
$env:ANDROID_NDK_ROOT = "D:\AppData\Android\sdk\ndk\30.0.14904198"
$env:PATH = "$env:ANDROID_NDK_ROOT;" +
            "$env:ANDROID_NDK_ROOT\toolchains\llvm\prebuilt\windows-x86_64\bin;" +
            "$env:ANDROID_NDK_ROOT\prebuilt\windows-x86_64\bin;" +
            "$env:PATH"
$env:CC = "aarch64-linux-android33-clang"
make module
```

### 打包为 zip（分发包）

```powershell
$env:ANDROID_NDK_ROOT = "D:\AppData\Android\sdk\ndk\30.0.14904198"
$env:PATH = "$env:ANDROID_NDK_ROOT\toolchains\llvm\prebuilt\windows-x86_64\bin;" +
            "$env:ANDROID_NDK_ROOT\prebuilt\windows-x86_64\bin;" +
            "$env:PATH"
$env:CC = "aarch64-linux-android33-clang"
make package
```

输出到 `clipsync-daemon/dist/clipsyncd-module.zip`。

### 注意：验证 zygisk .so 是否真的被更新

`make module` 里的 `copy /Y` 在 PowerShell 下偶尔不会真正覆盖 `module/zygisk/arm64-v8a.so`。编译后务必检查 md5：

```powershell
Get-FileHash -Path clipsync-daemon\zygisk\libs\arm64-v8a\libzygisk_clipsync.so, `
               clipsync-daemon\module\zygisk\arm64-v8a.so -Algorithm MD5
```

如果不一致，手动复制：

```powershell
Copy-Item -Path clipsync-daemon\zygisk\libs\arm64-v8a\libzygisk_clipsync.so `
          -Destination clipsync-daemon\module\zygisk\arm64-v8a.so -Force
```

## 安装模块到手机

推荐先把模块推到临时目录，再用 `su` 复制到 `/data/adb/modules/clipsyncd`，避免 adb 目录嵌套问题。

```powershell
# 1. 清理旧文件
adb shell "su -c 'rm -rf /data/local/tmp/clipsyncd-module /data/adb/modules/clipsyncd'"

# 2. 推送模块内容（注意是 module/. 不是 module）
adb push clipsync-daemon/module/. /data/local/tmp/clipsyncd-module/

# 3. 复制到 KernelSU 模块目录并设置权限
adb shell "su -c 'cp -r /data/local/tmp/clipsyncd-module /data/adb/modules/clipsyncd && chmod -R 0755 /data/adb/modules/clipsyncd'"

# 4. 验证结构
adb shell "su -c 'cd /data/adb/modules/clipsyncd && find . -maxdepth 2 | sort'"

# 5. 重启
adb reboot
```

## 重启后验证流程

### 1. 确认模块被 Zygisk 加载

```powershell
adb shell "su -c 'dmesg | grep clipsyncd'"
```

期望看到：

```
loaded 64bit zygisk module clipsyncd
```

### 2. 确认模块 .so 映射进了 system_server

```powershell
$ss_pid = adb shell "su -c 'pidof system_server'"
adb shell "su -c 'cat /proc/$ss_pid/maps'" | Select-String "clipsyncd"
```

期望看到 `/data/adb/modules/clipsyncd/zygisk/arm64-v8a.so` 出现在 system_server 的 maps 里。

### 3. 查看 Zygisk 模块日志

模块日志 tag 为 `ClipSyncBridge`，同时尝试写 `/data/adb/modules/clipsyncd/bridge.log` 和 `/dev/kmsg`：

```powershell
# logcat
adb shell "su -c 'logcat -d -f /data/local/tmp/full_logcat.txt; chmod 666 /data/local/tmp/full_logcat.txt'"
adb pull /data/local/tmp/full_logcat.txt C:\Users\$env:USERNAME\AppData\Local\Temp\full_logcat.txt
Select-String -Path C:\Users\$env:USERNAME\AppData\Local\Temp\full_logcat.txt `
              -Pattern "ClipSyncBridge|postServerSpecialize|preServerSpecialize" -CaseSensitive:$false

# 文件日志
adb shell "su -c 'cat /data/adb/modules/clipsyncd/bridge.log 2>/dev/null || echo no bridge.log'"

# 内核日志（kmsg）
adb shell "su -c 'dmesg > /data/local/tmp/dmesg.txt; chmod 666 /data/local/tmp/dmesg.txt'"
adb pull /data/local/tmp/dmesg.txt C:\Users\$env:USERNAME\AppData\Local\Temp\dmesg.txt
Select-String -Path C:\Users\$env:USERNAME\AppData\Local\Temp\dmesg.txt `
              -Pattern "ClipSyncBridge|clipbridge|postServerSpecialize" -CaseSensitive:$false
```

### 4. 确认 abstract socket 是否存在

```powershell
adb shell "su -c 'cat /proc/net/unix | grep clipbridge || echo no clipbridge socket'"
```

期望看到 `@clipbridge`（在 `/proc/net/unix` 中 abstract socket 名字可能显示为空格或 `@` 前缀）。

### 5. 手动运行 daemon 看连接是否成功

```powershell
adb shell "su -c 'pkill clipsyncd; /data/adb/modules/clipsyncd/system/bin/clipsyncd'"
```

这会前台运行，直接看输出。按 `Ctrl+C` 停止。

### 已知风险点

1. 由于手机内核限制， PC multicast 查询无法到达手机。因此目前只依靠手机 主动发包到pc实现
2. 手机开热点时，Android 策略路由默认让 mDNS 组播包（`224.0.0.251`）走移动数据接口（`rmnet_data*`）而非 SoftAP 接口（`wlan2`/`softap0`），导致连热点的 PC 收不到 mDNS 通告。`softap_detect.c` 在 `mdns_publish_init` 时检测 SoftAP 接口并向 `local_network` 策略表添加 `224.0.0.0/24 dev <softap>` 路由以修正此问题。检测逻辑：接口名匹配 AOSP `tetherableWifiRegexs`（`wlan\d`、`softap\d`、`ap_br_wlan\d`、`ap_br_softap\d`）+ `IFF_UP` + 有 IPv4 + `/proc/net/route` 中有直连（非网关、非默认）路由。

## 通知测试

### 模拟通知按钮点击验证回调

```powershell
# 模拟用户点击 action_id=1 的按钮
adb shell "su -c 'am broadcast -a dev.clipsync.NOTIF_ACTION --ei action_id 1'"
```

