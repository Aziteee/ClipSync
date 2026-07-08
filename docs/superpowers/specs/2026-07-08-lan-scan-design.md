# LAN Scan 功能设计

## 概述

为 PC 端（`clipsync-pc`）新增局域网主动扫描功能，作为现有 mDNS 被动监听的补充发现手段。每 3 分钟（可配置）扫描本机所在 /24 网段，主动探测 ClipSync 设备，与 mDNS 共存互补。

## 背景与动机

当前 PC 端只做**被动 mDNS 监听**（`mdns.rs` 绑定 `224.0.0.251:5353` 等 Android 主动发包）。AGENTS.md 提到的 `/24` unicast 探测 fallback 尚未实现。

在部分网络环境（如手机开热点）下，mDNS 组播包无法到达 PC，导致 mDNS 监听收不到 Android 通告。LAN scan 主动探测可在此场景下作为兜底发现手段。

## 目标

- 与 mDNS 被动监听共存，互不干扰
- 定期扫描本机所有非 loopback 接口的 /24 网段
- 复用现有设备注册、去重、重试链路
- 可通过配置控制开关与频率

## 非目标

- 不替代 mDNS，不修改 Android 端任何代码
- 不扫描非 /24 网段（如 /16、/8）
- 不引入新的设备 origin 类型

## 架构与数据流

```
run_sync_loop (main.rs)
  ├── mDNS 被动监听 (现有, mdns.rs)
  │     └─> DiscoveredEndpoint ─> register_device
  └── LAN scan 定时器 (新增, lan_scan.rs)
        ├─ 启动时立即扫一次
        ├─ 每 lan_scan_interval 秒扫一次 (若 > 0)
        └─> Vec<DiscoveredEndpoint> ─> 逐个 register_device (去重靠 URI hash)
```

- `lan_scan.rs` 暴露 `pub async fn scan_lan(port: u16, secret: &str, handshake_timeout: Duration) -> Vec<DiscoveredEndpoint>`
- 扫描流程：取本机所有非 loopback/链路本地 IPv4 接口 → 对每个接口算 /24 网段 → 跳过网络地址、广播地址、本机地址 → 对剩余 253 个 IP 并发 `TcpStream::connect(ip:port)`，超时 500ms，并发上限 64 → 连上的走完整 WebSocket 握手（`ws::connect_and_auth`），握手成功即认定为 ClipSync 设备 → 失败/超时直接丢弃，不影响其他
- `DiscoveredEndpoint` 的 `uri` 用 `ws://ip:port/ws`，`name` 为空，`service_name` 填 `"lan-scan"` 标识来源
- 注册时复用现有 `target_from_discovered` + `register_device`，URI 去重天然生效（mDNS 先注册的同 URI 设备不会被覆盖）

## 配置项

在 `config.rs` 的 `ConnectionConfig` 新增字段：

```rust
pub struct ConnectionConfig {
    pub port: u16,
    pub host: Option<String>,
    pub uri: Option<String>,
    pub heartbeat_interval_ms: u64,
    pub heartbeat_timeout_ms: u64,
    #[serde(default = "default_lan_scan_interval")]
    pub lan_scan_interval: i64,  // 新增
}
```

语义：
- `-1`：关闭 LAN scan
- `0`：仅启动时扫描一次，之后不再扫
- 正数 `N`：启动时扫一次，之后每 `N` 秒扫一次

默认值：`180`（3 分钟）

TOML 示例：
```toml
[connection]
port = 5287
lan_scan_interval = 180  # 默认 3 分钟；0=仅启动一次；-1=关闭
```

运行时机：仅 `auto_discovery` 模式（无静态设备）下生效，与 mDNS 同条件。即使 `lan_scan_interval = -1` 关闭 LAN scan，mDNS 被动监听仍正常运行。

## lan_scan.rs 模块设计

### 公共接口

```rust
pub async fn scan_lan(port: u16, secret: &str, handshake_timeout: Duration) -> Vec<DiscoveredEndpoint>;
```

### 内部结构

```
scan_lan(port, secret, handshake_timeout)
  ├─ collect_lan_subnets() -> Vec<(Ipv4Addr, Ipv4Addr)>  // (iface_ip, subnet_base)
  │     └─ 复用 local_ip_address crate 枚举接口
  │        过滤: 非 loopback / 非 link-local / 非 multicast
  │        每个 IPv4 接口算 /24 网络地址
  ├─ candidate_ips(subnets, local_ips) -> Vec<Ipv4Addr>
  │     └─ 展开每个 /24 为 254 个地址，跳过 .0 / .255 / 本机 IP
  └─ probe_all(candidates, port, secret, handshake_timeout) -> Vec<DiscoveredEndpoint>
        └─ 用 futures_util::stream::iter(candidates)
           .map(|ip| probe_one(ip, port, secret, handshake_timeout))
           .buffer_unordered(64)  // 并发 64
           .filter_map(|r| async move { r.ok() })
           .collect()
```

### probe_one 逻辑

```rust
async fn probe_one(ip: Ipv4Addr, port: u16, secret: &str, handshake_timeout: Duration) -> anyhow::Result<DiscoveredEndpoint> {
    let uri = format!("ws://{}:{}/ws", ip, port);
    // 复用 ws::connect_and_auth 完成 TCP connect + WebSocket 升级 + hello + HMAC auth
    let ws = ws::connect_and_auth(&uri, secret, handshake_timeout).await?;
    // 握手成功即认定是 ClipSync 设备且密钥一致，立即关闭连接
    drop(ws);
    Ok(DiscoveredEndpoint {
        uri,
        name: String::new(),
        service_name: "lan-scan".to_string(),
    })
}
```

使用真实 `cfg.auth.secret`：探测设备若 secret 不匹配会返回 `auth_fail`，`connect_and_auth` 返回 Err，被当作非 ClipSync 设备丢弃。LAN scan 只能发现 secret 一致的设备。

### 错误处理

- TCP connect 超时 / 连接被拒 → `Err`，丢弃
- `connect_async` 失败（非 WebSocket 服务）→ `Err`，丢弃
- `hello` 超时或非 ClipSync 协议 → `Err`，丢弃
- `auth_fail`（secret 不匹配）→ `Err`，丢弃
- 全部异常静默丢弃，仅 `log::debug!` 记录，不产生噪音

### 与 mDNS 的去重

- LAN scan 返回 `Vec<DiscoveredEndpoint>`，在 `main.rs` 逐个调 `target_from_discovered` + `register_device`
- `register_device` 已有 `contains_key` 检查，mDNS 先注册的同 URI 设备不会被覆盖
- LAN scan 发现的设备 `origin = Discovered`，复用现有 3 次重试上限；失败被移除后，下一轮 scan 会重新探测到并重新注册

## main.rs 集成

### run_sync_loop 改动

在现有 `auto_discovery` 分支里新增 LAN scan 定时器：

```rust
// 现有
let mut discovery_rx = if auto_discovery { start_discovery(...) } else { None };
let mut discovery_retry_sleep: Pin<Box<Sleep>> = ...;

// 新增
let lan_scan_interval = cfg.connection.lan_scan_interval;  // i64
let lan_scan_enabled = lan_scan_interval >= 0;  // -1 关闭
let mut lan_scan_sleep: Pin<Box<Sleep>> = Box::pin(tokio::time::sleep(Duration::MAX));
if auto_discovery && lan_scan_enabled {
    lan_scan_sleep.as_mut().reset(Instant::now());  // 启动时立即扫一次
}
```

### select! 新增分支

```rust
tokio::select! {
    // ... 现有 mDNS 分支 ...
    _ = &mut lan_scan_sleep, if auto_discovery && lan_scan_enabled => {
        let port = cfg.connection.port;
        let secret = cfg.auth.secret.clone();
        let hs_timeout = handshake_timeout(&cfg);
        tokio::spawn(async move {
            let endpoints = lan_scan::scan_lan(port, &secret, hs_timeout).await;
            let _ = lan_scan_tx.send(endpoints);
        });
        // 安排下一轮
        if lan_scan_interval > 0 {
            lan_scan_sleep.as_mut().reset(Instant::now() + Duration::from_secs(lan_scan_interval as u64));
        } else {
            // interval == 0: 仅启动一次，停用定时器
            lan_scan_sleep.as_mut().reset(Instant::now() + Duration::MAX);
        }
    }
    // ... 现有 clip_rx / debounce / device_event 分支 ...
}
```

### 结果回传通道

LAN scan 在独立 task 里运行（避免阻塞 select!），通过新增的 `lan_scan_rx` 把 `Vec<DiscoveredEndpoint>` 送回 `run_sync_loop`：

```rust
let (lan_scan_tx, mut lan_scan_rx) = mpsc::unbounded_channel::<Vec<mdns::DiscoveredEndpoint>>();

// select! 新增
Some(endpoints) = lan_scan_rx.recv() => {
    let mut new_any = false;
    for endpoint in endpoints {
        let target = target_from_discovered(endpoint);
        if register_device(target, &mut handles, cfg.clone(), device_event_tx.clone(), &mut engine, latest_text.as_deref()) {
            new_any = true;
        }
    }
    if new_any {
        send_aggregate_state(&proxy, &handles);
    }
}
```

## 测试策略

### config.rs 单元测试

- `lan_scan_interval` 默认值 `180`
- TOML 解析：`lan_scan_interval = 0` / `-1` / 正数
- 缺省时取默认 `180`

### lan_scan.rs 单元测试

- `collect_lan_subnets`：mock 接口列表，验证过滤 loopback/link-local/multicast，正确算 /24 网络地址
- `candidate_ips`：给定 `(192.168.1.5, 192.168.1.0)`，验证展开 254 个 IP 且跳过 `.0`/`.255`/本机 `192.168.1.5`
- `probe_one`：用 `tokio::net::TcpListener` 起本地模拟服务
  - 起一个返回 `hello` + 接受 auth 的真 ClipSync 模拟服务 → 探测成功
  - 起一个只 accept 不回 hello 的服务 → 探测失败
  - 起一个回非 ClipSync 协议的服务 → 探测失败
- `scan_lan` 集成测试：起本地模拟 ClipSync 服务在某个端口，`scan_lan` 能发现它。由于 `scan_lan` 默认过滤 loopback，测试需通过 `probe_one` 直接验证，或暴露带 `include_loopback: bool` 的内部测试入口

### main.rs 现有测试不受影响

- `configured_device_targets` / `register_device` / 去重逻辑不变
- 新增 `lan_scan_sleep` 与 `lan_scan_rx` 分支，现有 select! 分支行为不变

## 依赖

- 复用现有 crate：`tokio`（`TcpStream`、`time::timeout`）、`futures-util`（`stream::iter`、`buffer_unordered`）、`local-ip-address`（已在 `mdns.rs` 使用）
- 不新增第三方依赖

## 影响范围

| 文件 | 改动 |
|------|------|
| `clipsync-pc/src/config.rs` | `ConnectionConfig` 新增 `lan_scan_interval` 字段 + 默认值 + 测试 |
| `clipsync-pc/src/lan_scan.rs` | 新建模块：`scan_lan` / `probe_one` / `collect_lan_subnets` / `candidate_ips` + 测试 |
| `clipsync-pc/src/main.rs` | `mod lan_scan;` + `run_sync_loop` 新增 `lan_scan_sleep` / `lan_scan_rx` 分支 |

不影响 Android 端（`clipsync-daemon`）任何代码。
