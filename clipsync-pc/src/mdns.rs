use mdns_sd::{ServiceDaemon, ServiceEvent};
use std::net::IpAddr;
use std::time::Duration;
use tokio::sync::mpsc as tokio_mpsc;

const SERVICE_TYPE: &str = "_clipsync._tcp.local.";

pub fn discover(_port: u16) -> anyhow::Result<tokio_mpsc::UnboundedReceiver<String>> {
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

    Ok(rx)
}

fn ws_uri(addr: IpAddr, port: u16) -> String {
    match addr {
        IpAddr::V4(addr) => format!("ws://{}:{}/ws", addr, port),
        IpAddr::V6(addr) => format!("ws://[{}]:{}/ws", addr, port),
    }
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
}
