use crate::protocol::ClipMessage;
use mdns_sd::{ServiceDaemon, ServiceEvent};
use std::collections::HashSet;
use std::io::{Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, SocketAddrV4, TcpStream};
use std::sync::{mpsc as std_mpsc, Arc, Mutex};
use std::time::Duration;
use tokio::sync::mpsc as tokio_mpsc;

const SERVICE_TYPE: &str = "_clipsync._tcp.local.";
const LAN_SCAN_INTERVAL: Duration = Duration::from_secs(15);
const LAN_SCAN_MAX_INTERVAL: Duration = Duration::from_secs(120);
const LAN_SCAN_CONNECT_TIMEOUT: Duration = Duration::from_millis(250);
const LAN_SCAN_WORKERS: usize = 32;

pub fn discover(port: u16) -> anyhow::Result<tokio_mpsc::UnboundedReceiver<String>> {
    let (tx, rx) = tokio_mpsc::unbounded_channel();

    match ServiceDaemon::new() {
        Ok(daemon) => match daemon.browse(SERVICE_TYPE) {
            Ok(browser) => {
                let mdns_tx = tx.clone();
                std::thread::spawn(move || loop {
                    if mdns_tx.is_closed() {
                        break;
                    }
                    match browser.recv() {
                        Ok(ServiceEvent::ServiceResolved(info)) => {
                            if let Some(addr) = info.get_addresses().iter().next().cloned() {
                                let uri = ws_uri(addr, info.get_port());
                                log::info!("mDNS discovered: {}", uri);
                                let _ = mdns_tx.send(uri);
                            }
                        }
                        Ok(_) => {}
                        Err(e) => {
                            log::error!("mDNS browse error: {}", e);
                            break;
                        }
                    }
                });

                let rebrowse_tx = tx.clone();
                std::thread::spawn(move || loop {
                    std::thread::sleep(Duration::from_secs(30));
                    if rebrowse_tx.is_closed() {
                        break;
                    }
                    let _ = daemon.browse(SERVICE_TYPE);
                });
            }
            Err(e) => {
                log::error!("mDNS browse setup failed: {}", e);
            }
        },
        Err(e) => {
            log::error!("mDNS daemon setup failed: {}", e);
        }
    }

    let _ = port;
    Ok(rx)
}

fn spawn_lan_scan(tx: tokio_mpsc::UnboundedSender<String>, port: u16) {
    std::thread::spawn(move || {
        let mut interval = LAN_SCAN_INTERVAL;
        loop {
            if tx.is_closed() {
                break;
            }
            let mut found_any = false;
            for uri in scan_open_lan_uris(port, LAN_SCAN_CONNECT_TIMEOUT) {
                found_any = true;
                log::info!("LAN scan discovered: {}", uri);
                if tx.send(uri).is_err() {
                    return;
                }
            }
            std::thread::sleep(interval);
            interval = next_lan_scan_interval(interval, found_any);
        }
    });
}

fn next_lan_scan_interval(current: Duration, found_any: bool) -> Duration {
    if found_any {
        LAN_SCAN_INTERVAL
    } else {
        (current * 2).min(LAN_SCAN_MAX_INTERVAL)
    }
}

fn scan_open_lan_uris(port: u16, timeout: Duration) -> Vec<String> {
    let mut hosts = HashSet::new();
    for local_addr in local_ipv4_addrs() {
        hosts.extend(subnet_scan_hosts(local_addr));
    }

    let (work_tx, work_rx) = std_mpsc::channel::<Ipv4Addr>();
    let work_rx = Arc::new(Mutex::new(work_rx));
    let (found_tx, found_rx) = std_mpsc::channel::<Ipv4Addr>();
    let mut workers = Vec::with_capacity(LAN_SCAN_WORKERS);

    for _ in 0..LAN_SCAN_WORKERS {
        let work_rx = Arc::clone(&work_rx);
        let found_tx = found_tx.clone();
        workers.push(std::thread::spawn(move || loop {
            let host = {
                let rx = work_rx.lock().expect("LAN scan worker mutex poisoned");
                rx.recv()
            };
            let Ok(host) = host else {
                break;
            };
            if probe_clipsync_ws(host, port, timeout) {
                let _ = found_tx.send(host);
            }
        }));
    }
    drop(found_tx);

    for host in hosts {
        let _ = work_tx.send(host);
    }
    drop(work_tx);

    for worker in workers {
        let _ = worker.join();
    }

    found_rx
        .try_iter()
        .map(|addr| ws_uri(IpAddr::V4(addr), port))
        .collect()
}

fn local_ipv4_addrs() -> Vec<Ipv4Addr> {
    match local_ip_address::list_afinet_netifas() {
        Ok(ifaces) => ifaces
            .into_iter()
            .filter_map(|(_, addr)| match addr {
                IpAddr::V4(addr) if is_scannable_ipv4(addr) => Some(addr),
                _ => None,
            })
            .collect(),
        Err(e) => {
            log::error!("LAN scan interface enumeration failed: {}", e);
            Vec::new()
        }
    }
}

fn subnet_scan_hosts(local_addr: Ipv4Addr) -> Vec<Ipv4Addr> {
    if !is_scannable_ipv4(local_addr) {
        return Vec::new();
    }

    let octets = local_addr.octets();
    (1..=254)
        .map(|host| Ipv4Addr::new(octets[0], octets[1], octets[2], host))
        .filter(|addr| *addr != local_addr)
        .collect()
}

#[cfg(test)]
fn subnet_scan_uris(local_addr: Ipv4Addr, port: u16) -> Vec<String> {
    subnet_scan_hosts(local_addr)
        .into_iter()
        .map(|addr| ws_uri(IpAddr::V4(addr), port))
        .collect()
}

fn is_scannable_ipv4(addr: Ipv4Addr) -> bool {
    let octets = addr.octets();
    match octets {
        [10, _, _, _] => true,
        [172, second, _, _] if (16..=31).contains(&second) => true,
        [192, 168, _, _] => true,
        _ => false,
    }
}

fn ws_uri(addr: IpAddr, port: u16) -> String {
    match addr {
        IpAddr::V4(addr) => format!("ws://{}:{}/ws", addr, port),
        IpAddr::V6(addr) => format!("ws://[{}]:{}/ws", addr, port),
    }
}

fn probe_clipsync_ws(host: Ipv4Addr, port: u16, timeout: Duration) -> bool {
    let addr = SocketAddr::V4(SocketAddrV4::new(host, port));
    let Ok(mut stream) = TcpStream::connect_timeout(&addr, timeout) else {
        return false;
    };
    let _ = stream.set_read_timeout(Some(timeout));
    let _ = stream.set_write_timeout(Some(timeout));

    let request = format!(
        "GET /ws HTTP/1.1\r\n\
         Host: {host}:{port}\r\n\
         Upgrade: websocket\r\n\
         Connection: Upgrade\r\n\
         Sec-WebSocket-Version: 13\r\n\
         Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\
         \r\n"
    );
    if stream.write_all(request.as_bytes()).is_err() {
        return false;
    }

    let mut response = Vec::with_capacity(2048);
    let mut chunk = [0u8; 512];
    while response.len() < 2048 {
        let Ok(n) = stream.read(&mut chunk) else {
            return false;
        };
        if n == 0 {
            return false;
        }
        response.extend_from_slice(&chunk[..n]);
        if is_clipsync_probe_response(&response) {
            return true;
        }
        if has_non_switching_protocol_response(&response) {
            return false;
        }
    }
    false
}

fn is_clipsync_probe_response(response: &[u8]) -> bool {
    let Some(header_end) = find_subslice(response, b"\r\n\r\n") else {
        return false;
    };
    let headers = String::from_utf8_lossy(&response[..header_end]);
    if !headers.starts_with("HTTP/1.1 101") && !headers.starts_with("HTTP/1.0 101") {
        return false;
    }

    let frame = &response[header_end + 4..];
    let Some(payload) = websocket_text_payload(frame) else {
        return false;
    };
    let Ok(text) = std::str::from_utf8(payload) else {
        return false;
    };
    matches!(ClipMessage::from_json(text), Ok(ClipMessage::Hello { .. }))
}

fn has_non_switching_protocol_response(response: &[u8]) -> bool {
    let Some(header_end) = find_subslice(response, b"\r\n\r\n") else {
        return false;
    };
    let headers = String::from_utf8_lossy(&response[..header_end]);
    !headers.starts_with("HTTP/1.1 101") && !headers.starts_with("HTTP/1.0 101")
}

fn websocket_text_payload(frame: &[u8]) -> Option<&[u8]> {
    if frame.len() < 2 || frame[0] & 0x0f != 0x01 {
        return None;
    }
    let masked = frame[1] & 0x80 != 0;
    if masked {
        return None;
    }

    let len_byte = frame[1] & 0x7f;
    let (payload_offset, payload_len) = match len_byte {
        0..=125 => (2usize, len_byte as usize),
        126 => {
            if frame.len() < 4 {
                return None;
            }
            (4usize, u16::from_be_bytes([frame[2], frame[3]]) as usize)
        }
        127 => {
            if frame.len() < 10 {
                return None;
            }
            let len = u64::from_be_bytes([
                frame[2], frame[3], frame[4], frame[5], frame[6], frame[7], frame[8], frame[9],
            ]);
            if len > usize::MAX as u64 {
                return None;
            }
            (10usize, len as usize)
        }
        _ => return None,
    };

    frame.get(payload_offset..payload_offset + payload_len)
}

fn find_subslice(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack
        .windows(needle.len())
        .position(|window| window == needle)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};

    #[test]
    fn ws_uri_includes_websocket_path_and_brackets_ipv6() {
        assert_eq!(
            ws_uri(IpAddr::V4(Ipv4Addr::new(192, 168, 0, 103)), 5287),
            "ws://192.168.0.103:5287/ws"
        );
        assert_eq!(
            ws_uri(IpAddr::V6(Ipv6Addr::LOCALHOST), 5287),
            "ws://[::1]:5287/ws"
        );
    }

    #[test]
    fn subnet_scan_candidates_cover_same_24_without_self_or_edges() {
        let uris = subnet_scan_uris(Ipv4Addr::new(192, 168, 0, 104), 5287);

        assert_eq!(uris.len(), 253);
        assert!(uris.contains(&"ws://192.168.0.103:5287/ws".to_string()));
        assert!(!uris.contains(&"ws://192.168.0.0:5287/ws".to_string()));
        assert!(!uris.contains(&"ws://192.168.0.104:5287/ws".to_string()));
        assert!(!uris.contains(&"ws://192.168.0.255:5287/ws".to_string()));
    }

    #[test]
    fn subnet_scan_candidates_skip_non_lan_addresses() {
        assert!(subnet_scan_uris(Ipv4Addr::new(127, 0, 0, 1), 5287).is_empty());
        assert!(subnet_scan_uris(Ipv4Addr::new(169, 254, 10, 20), 5287).is_empty());
        assert!(subnet_scan_uris(Ipv4Addr::new(100, 64, 1, 2), 5287).is_empty());
    }

    #[test]
    fn probe_response_requires_websocket_hello() {
        let hello = br#"{"type":"hello","challenge":"abc"}"#;
        let mut response =
            b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n".to_vec();
        response.push(0x81);
        response.push(hello.len() as u8);
        response.extend_from_slice(hello);

        assert!(is_clipsync_probe_response(&response));
        assert!(!is_clipsync_probe_response(
            b"HTTP/1.1 200 OK\r\n\r\nnot websocket"
        ));
        assert!(!is_clipsync_probe_response(
            b"HTTP/1.1 101 Switching Protocols\r\n\r\n\x81\x0fnot clipsync"
        ));
    }

    #[test]
    fn lan_scan_interval_backs_off_when_no_hosts_are_found() {
        assert_eq!(
            next_lan_scan_interval(LAN_SCAN_INTERVAL, false),
            Duration::from_secs(30)
        );
    }

    #[test]
    fn lan_scan_interval_resets_after_discovery() {
        assert_eq!(
            next_lan_scan_interval(Duration::from_secs(120), true),
            LAN_SCAN_INTERVAL
        );
    }
}
