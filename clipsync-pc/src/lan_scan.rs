use crate::mdns::DiscoveredEndpoint;
use crate::ws;
use futures_util::stream::{self, StreamExt};
use std::net::{IpAddr, Ipv4Addr};
use std::time::Duration;

#[cfg(windows)]
use windows::Win32::NetworkManagement::IpHelper::{
    GetIfTable2, FreeMibTable, MIB_IF_TABLE2, MIB_IF_ROW2,
};
#[cfg(windows)]
use windows::Win32::Foundation::NO_ERROR;

const PROBE_TIMEOUT: Duration = Duration::from_secs(3);
const CONCURRENCY: usize = 64;
const SERVICE_NAME: &str = "lan-scan";

pub async fn scan_lan(port: u16, secret: &str, handshake_timeout: Duration) -> Vec<DiscoveredEndpoint> {
    let subnets = collect_lan_subnets();
    if subnets.is_empty() {
        log::info!("LAN scan: no usable IPv4 interfaces found");
        return Vec::new();
    }

    let local_ips: Vec<Ipv4Addr> = subnets.iter().map(|(ip, _)| *ip).collect();
    let candidates = candidate_ips(&subnets, &local_ips);
    log::info!(
        "LAN scan: probing {} candidate addresses on port {} ({} subnets)",
        candidates.len(),
        port,
        subnets.len()
    );

    let secret = secret.to_string();
    stream::iter(candidates)
        .map(|ip| probe_one(ip, port, secret.clone(), handshake_timeout))
        .buffer_unordered(CONCURRENCY)
        .filter_map(|r| async move { r.ok() })
        .collect()
        .await
}

fn collect_lan_subnets() -> Vec<(Ipv4Addr, Ipv4Addr)> {
    let physical_names = physical_interface_names();
    let ifaces = local_ip_address::list_afinet_netifas().unwrap_or_default();
    let mut subnets = Vec::new();
    for (name, addr) in ifaces {
        if let IpAddr::V4(ip) = addr {
            if usable_scan_interface(ip) && physical_names.contains(name.as_str()) {
                log::debug!("LAN scan: using iface {} ({})", name, ip);
                let base = subnet_base_24(ip);
                subnets.push((ip, base));
            }
        }
    }
    subnets.sort_unstable();
    subnets.dedup();
    subnets
}

#[cfg(windows)]
fn physical_interface_names() -> std::collections::HashSet<String> {
    let mut names = std::collections::HashSet::new();
    unsafe {
        let mut table_ptr: *mut MIB_IF_TABLE2 = std::ptr::null_mut();
        if GetIfTable2(&mut table_ptr) != NO_ERROR {
            log::warn!("LAN scan: GetIfTable2 failed");
            return names;
        }
        let table = &*table_ptr;
        for i in 0..table.NumEntries {
            let row: &MIB_IF_ROW2 = &*table.Table.as_ptr().add(i as usize);
            let iftype = row.Type;
            let oper_up = row.OperStatus.0 == 1;
            let is_ethernet_or_wifi = iftype == 6 || iftype == 71;
            let alias = utf16_to_string(&row.Alias);
            let desc = utf16_to_string(&row.Description);
            // Filter rows have alias like "WLAN-WFP...", skip those (they have no IP anyway)
            let is_filter_row = alias.contains('-');
            let is_virtual_name = alias.starts_with("vEthernet")
                || alias.starts_with("vSwitch")
                || alias.starts_with("Loopback")
                || alias.starts_with("本地连接*")
                || alias.starts_with("蓝牙");
            if is_ethernet_or_wifi && oper_up && !is_filter_row && !is_virtual_name {
                names.insert(alias);
                names.insert(desc);
            }
        }
        FreeMibTable(table_ptr as *const _ as _);
    }
    log::debug!("LAN scan: physical interface names: {:?}", names);
    names
}

#[cfg(windows)]
fn utf16_to_string(buf: &[u16]) -> String {
    let len = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    String::from_utf16_lossy(&buf[..len])
}

#[cfg(not(windows))]
fn physical_interface_names() -> std::collections::HashSet<String> {
    let mut names = std::collections::HashSet::new();
    let ifaces = local_ip_address::list_afinet_netifas().unwrap_or_default();
    for (name, _) in ifaces {
        if name != "lo" && !name.starts_with("docker") && !name.starts_with("veth") {
            names.insert(name);
        }
    }
    names
}

fn usable_scan_interface(addr: Ipv4Addr) -> bool {
    !addr.is_unspecified()
        && !addr.is_loopback()
        && !addr.is_link_local()
        && !addr.is_broadcast()
        && !addr.is_multicast()
}

fn subnet_base_24(ip: Ipv4Addr) -> Ipv4Addr {
    let octets = ip.octets();
    Ipv4Addr::new(octets[0], octets[1], octets[2], 0)
}

fn candidate_ips(subnets: &[(Ipv4Addr, Ipv4Addr)], local_ips: &[Ipv4Addr]) -> Vec<Ipv4Addr> {
    let mut seen = std::collections::HashSet::new();
    let mut out = Vec::new();
    for (_, base) in subnets {
        let base_octets = base.octets();
        for host in 1u8..=254u8 {
            let ip = Ipv4Addr::new(base_octets[0], base_octets[1], base_octets[2], host);
            if local_ips.contains(&ip) {
                continue;
            }
            if seen.insert(ip) {
                out.push(ip);
            }
        }
    }
    out
}

async fn probe_one(
    ip: Ipv4Addr,
    port: u16,
    secret: String,
    handshake_timeout: Duration,
) -> anyhow::Result<DiscoveredEndpoint> {
    let uri = format!("ws://{}:{}/ws", ip, port);
    let effective_hs = handshake_timeout.min(PROBE_TIMEOUT);
    let connect = async { ws::connect_and_auth(&uri, &secret, effective_hs).await };
    let ws = tokio::time::timeout(PROBE_TIMEOUT, connect)
        .await
        .map_err(|_| anyhow::anyhow!("probe timeout"))??;
    drop(ws);

    log::debug!("LAN scan: discovered ClipSync device at {}", uri);
    Ok(DiscoveredEndpoint {
        uri,
        name: String::new(),
        service_name: SERVICE_NAME.to_string(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::ClipMessage;
    use futures_util::SinkExt;
    use hmac::Mac;
    use std::net::SocketAddr;
    use tokio::net::TcpListener;
    use tokio_tungstenite::accept_async;
    use tokio_tungstenite::tungstenite::Message;

    #[test]
    fn subnet_base_24_zeroes_host_octet() {
        assert_eq!(
            subnet_base_24(Ipv4Addr::new(192, 168, 1, 5)),
            Ipv4Addr::new(192, 168, 1, 0)
        );
        assert_eq!(
            subnet_base_24(Ipv4Addr::new(10, 0, 0, 200)),
            Ipv4Addr::new(10, 0, 0, 0)
        );
    }

    #[test]
    fn usable_scan_interface_filters_non_routable() {
        assert!(usable_scan_interface(Ipv4Addr::new(192, 168, 1, 5)));
        assert!(usable_scan_interface(Ipv4Addr::new(10, 0, 0, 5)));
        assert!(!usable_scan_interface(Ipv4Addr::UNSPECIFIED));
        assert!(!usable_scan_interface(Ipv4Addr::LOCALHOST));
        assert!(!usable_scan_interface(Ipv4Addr::new(169, 254, 1, 2)));
        assert!(!usable_scan_interface(Ipv4Addr::new(224, 0, 0, 1)));
    }

    #[test]
    fn candidate_ips_skips_network_broadcast_and_local() {
        let subnets = vec![
            (Ipv4Addr::new(192, 168, 1, 5), Ipv4Addr::new(192, 168, 1, 0)),
        ];
        let local_ips = vec![Ipv4Addr::new(192, 168, 1, 5)];
        let ips = candidate_ips(&subnets, &local_ips);

        assert!(!ips.contains(&Ipv4Addr::new(192, 168, 1, 0)));
        assert!(!ips.contains(&Ipv4Addr::new(192, 168, 1, 255)));
        assert!(!ips.contains(&Ipv4Addr::new(192, 168, 1, 5)));
        assert!(ips.contains(&Ipv4Addr::new(192, 168, 1, 1)));
        assert!(ips.contains(&Ipv4Addr::new(192, 168, 1, 254)));
        assert_eq!(ips.len(), 253);
    }

    #[test]
    fn candidate_ips_dedupes_across_subnets() {
        let subnets = vec![
            (Ipv4Addr::new(192, 168, 1, 5), Ipv4Addr::new(192, 168, 1, 0)),
            (Ipv4Addr::new(192, 168, 1, 6), Ipv4Addr::new(192, 168, 1, 0)),
        ];
        let local_ips = vec![
            Ipv4Addr::new(192, 168, 1, 5),
            Ipv4Addr::new(192, 168, 1, 6),
        ];
        let ips = candidate_ips(&subnets, &local_ips);
        assert_eq!(ips.len(), 252);
    }

    async fn spawn_clipsync_server(secret: &str) -> (SocketAddr, tokio::task::JoinHandle<()>) {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        let secret = secret.to_string();
        let handle = tokio::spawn(async move {
            let (stream, _) = listener.accept().await.unwrap();
            let mut ws = accept_async(stream).await.unwrap();
            let challenge = "test-challenge".to_string();
            let hello = ClipMessage::Hello { challenge };
            ws.send(Message::Text(hello.to_json())).await.unwrap();

            let raw = ws.next().await.unwrap().unwrap();
            let text = match raw {
                Message::Text(t) => t,
                _ => panic!("expected text"),
            };
            let msg = ClipMessage::from_json(&text).unwrap();
            let received_response = match msg {
                ClipMessage::Auth { response } => response,
                _ => panic!("expected auth"),
            };

            let mut mac = hmac::Hmac::<sha2::Sha256>::new_from_slice(secret.as_bytes()).unwrap();
            mac.update(b"test-challenge");
            let expected = hex::encode(mac.finalize().into_bytes());

            if received_response == expected {
                ws.send(Message::Text(ClipMessage::AuthOk.to_json()))
                    .await
                    .unwrap();
            } else {
                ws.send(Message::Text(ClipMessage::AuthFail.to_json()))
                    .await
                    .unwrap();
            }
        });
        (addr, handle)
    }

    #[tokio::test]
    async fn probe_one_succeeds_for_valid_clipsync_server() {
        let secret = "shared-secret";
        let (addr, _handle) = spawn_clipsync_server(secret).await;
        let result = probe_one(
            Ipv4Addr::new(127, 0, 0, 1),
            addr.port(),
            secret.to_string(),
            Duration::from_secs(2),
        )
        .await;

        assert!(result.is_ok(), "probe should succeed: {:?}", result.err());
        let endpoint = result.unwrap();
        assert!(endpoint.uri.starts_with("ws://127.0.0.1"));
        assert_eq!(endpoint.name, "");
        assert_eq!(endpoint.service_name, "lan-scan");
    }

    #[tokio::test]
    async fn probe_one_fails_on_wrong_secret() {
        let secret = "shared-secret";
        let wrong = "wrong-secret";
        let (addr, _handle) = spawn_clipsync_server(secret).await;
        let result = probe_one(
            Ipv4Addr::new(127, 0, 0, 1),
            addr.port(),
            wrong.to_string(),
            Duration::from_secs(2),
        )
        .await;

        assert!(result.is_err());
    }

    #[tokio::test]
    async fn probe_one_fails_on_non_clipsync_server() {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        let _handle = tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            use tokio::io::AsyncReadExt;
            let mut buf = [0u8; 1024];
            let _ = stream.read(&mut buf).await;
        });

        let result = probe_one(
            Ipv4Addr::new(127, 0, 0, 1),
            addr.port(),
            "secret".to_string(),
            Duration::from_secs(2),
        )
        .await;

        assert!(result.is_err());
    }

    #[tokio::test]
    async fn probe_one_fails_on_closed_port() {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let port = listener.local_addr().unwrap().port();
        drop(listener);

        let result = probe_one(
            Ipv4Addr::new(127, 0, 0, 1),
            port,
            "secret".to_string(),
            Duration::from_secs(2),
        )
        .await;

        assert!(result.is_err());
    }
}
