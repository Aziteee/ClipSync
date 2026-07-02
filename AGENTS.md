# ClipSync Agent 调试手册

> 本文件记录 Android 端（KernelSU/Zygisk 模块 + clipsyncd 守护进程）的编译、安装、调试流程。

## 环境

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

# 5. 安全检查：当前版本不应再包含 initrc/clipsyncd.rc
adb shell "su -c 'test ! -e /data/adb/modules/clipsyncd/initrc/clipsyncd.rc && echo no initrc rc'"

# 6. 重启
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

## 当前调试状态（截至 2026-07-02 16:45）

- ✅ 已实现真实 Android WebSocket server，监听 `0.0.0.0:5287`。
- ✅ 已实现 HMAC handshake：`hello` challenge -> `auth` response -> `auth_ok/auth_fail`。
- ✅ 已实现协议 JSON helper，`clipboard_push` 会正确转义引号、反斜杠和换行。
- ✅ 已实现 `clipboard_set` 入站路由，认证后会调用 Zygisk bridge 写剪贴板。
- ✅ 已实现 Mongoose mDNS/DNS-SD publisher，UDP `5353` socket 存在。
- ✅ 已删除模块 `initrc/clipsyncd.rc`。不要再让 Android `init` 直接管理 `clipsyncd`；只通过 KernelSU `service.sh` 启动 daemon。
- ✅ 已修复 `clipboard_set` 导致 system_server 软重启的问题：Zygisk bridge 现在会按实际找到的 `IClipboard` JNI 签名分别调用新版/旧版 `setPrimaryClip` 和 `getPrimaryClip`。
- ✅ 当前已安装修复版模块。验证时 `system_server` PID `3597`、`clipsyncd` PID `7013` 在 3 分钟 `clipboard_set` 压测中保持不变。
- ⚠️ PC 客户端 mDNS 自动发现仍未验证通过：PC 侧发出的 multicast 查询没有到达手机 tcpdump。直连 WebSocket 已验证通过。

### 最近关键验证结果

```powershell
# daemon / socket / listener
adb shell "su -c 'pidof system_server; pidof clipsyncd'"
adb shell "su -c 'cat /proc/net/tcp /proc/net/tcp6 | grep -i :14A7 || echo no 5287 listener'"
adb shell "su -c 'cat /proc/net/unix | grep clipbridge || echo no clipbridge socket'"

# WebSocket smoke
cd clipsync-daemon
.\tools\ws_smoke.ps1 -Uri "ws://192.168.0.103:5287/ws" -Secret ""
```

期望：

```text
websocket smoke passed: ws://192.168.0.103:5287/ws
```

### 软重启根因记录

复现条件：

1. 安装旧版模块并重启。
2. PC/WebSocket 客户端认证成功。
3. 发送 `{"type":"clipboard_set","text":"..."}`。
4. 约 10 秒内 `system_server` PID 改变，表现为 framework 软重启。

根因：

- `zygisk/jni/main.cpp` 先查新版 `IClipboard.setPrimaryClip(ClipData, String, String, int, int)`。
- 找不到时 fallback 到旧版 `setPrimaryClip(ClipData, String, int)`。
- 旧代码无论找到哪个签名，都按新版 5 参数调用，导致 system_server 内 JNI 调用栈错误。
- `getPrimaryClip` 也有同类新旧签名调用问题，已一起修复。

止血命令：

```powershell
adb wait-for-device
adb shell "su -c 'rm -rf /data/adb/modules/clipsyncd /data/local/tmp/clipsyncd-module; pkill clipsyncd || true; sync'"
adb reboot
```

### 已知风险点

1. **不要恢复 `module/initrc/clipsyncd.rc`**：它会把 daemon 交给 Android `init` 管理，曾与 `service.sh` 双启动并增加软重启排查难度。
2. **`make module` 后必须核对 Zygisk `.so` MD5**：PowerShell 下 `copy /Y` 仍可能显示成功但没有覆盖。
3. **mDNS 自动发现未通**：Android 端 UDP `5353` socket 存在，但 PC multicast 查询未到达手机；优先从 Windows 防火墙/网络 multicast/AP 隔离方向查。
4. **真实 PC 客户端连接仍建议用直连 stress 先验证**：mDNS 不通时，使用 `tools/ws_smoke.ps1` 或自定义直连 WebSocket 客户端验证 `clipboard_set`。

## 推荐的下一步操作

1. 继续保持当前已安装修复版模块，观察是否还有无操作软重启。
2. 单独排查 mDNS：
   ```powershell
   adb shell "su -c 'cat /proc/net/udp /proc/net/udp6 | grep -i :14E9 || echo no mdns udp socket'"
   adb shell "su -c 'timeout 8 tcpdump -i any -n udp port 5353 -c 10'"
   ```
3. 如果 PC multicast 仍不到手机，先给 PC 客户端增加手动 IP fallback，再让完整剪贴板链路绕过 mDNS 继续验证。
4. 完成 PC 端连接后做端到端剪贴板验证：
   - PC -> phone：发送 `clipboard_set`，确认手机不软重启。
   - phone -> PC：手机剪贴板变化后确认 `clipboard_push` 广播。
5. 最后再决定是否需要 mDNS 的替代方案，例如手动 IP、UDP broadcast discovery，或用户配置固定地址。
