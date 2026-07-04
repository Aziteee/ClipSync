# ClipSync Agent 调试手册

> 本文件记录 Android 端（KernelSU/Zygisk 模块 + clipsyncd 守护进程）的编译、安装、调试流程。

## 环境

- 平台：Windows + PowerShell
- 已连接 adb 设备（示例设备号 `293b43e2`）
- Android NDK 路径：`D:\AppData\Android\sdk\ndk\30.0.14904198`

## 当前剪贴板访问架构

主同步链路不是 `clipsyncd` 直接使用 `libbinder_ndk`：

```text
clipsyncd -> @clipbridge abstract Unix socket -> Zygisk bridge in system_server -> IClipboard/ClipboardService
```

`IClipboard` 调用发生在注入 `system_server` 的 Zygisk bridge 内。`clipsyncd` 只通过 `@clipbridge` 发送 `READ` / `WRITE` 命令；调试 `@clipbridge` 仍按本文后续流程。

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

1. **`make module` 后必须核对 Zygisk `.so` MD5**：PowerShell 下 `copy /Y` 仍可能显示成功但没有覆盖。
2. **纯 mDNS multicast 仍可能不通**：Android 端 UDP `5353` socket 存在，但当前网络里 PC multicast 查询未到达手机；若要恢复纯 mDNS，优先从 Windows 防火墙/网络 multicast/AP 隔离方向查。
3. **PC 自动发现依赖 LAN unicast fallback 兜底**：`clipsync-pc/clipsync.toml` 当前不配置 `host`/`uri`。fallback 会探测同 `/24` 地址并要求 ClipSync WebSocket `hello`，可以过滤其他开放 `5287` 的服务。
4. **Zygisk bridge 读取依赖 ColorOS/Android 内部字段**：当前读取 fallback 使用 `ClipboardService.mClipboards` 的 `SparseArrayMap` 结构。系统升级后若字段结构变化，需要重新用 logcat 字段枚举确认。

