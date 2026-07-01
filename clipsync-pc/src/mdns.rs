use mdns_sd::{ServiceDaemon, ServiceEvent};
use std::time::Duration;
use tokio::sync::mpsc;

const SERVICE_TYPE: &str = "_clipsync._tcp.local.";

pub fn discover() -> anyhow::Result<mpsc::UnboundedReceiver<String>> {
    let (tx, rx) = mpsc::unbounded_channel();
    let daemon = ServiceDaemon::new()?;
    let browser = daemon.browse(SERVICE_TYPE)?;

    std::thread::spawn(move || loop {
        match browser.recv() {
            Ok(ServiceEvent::ServiceResolved(info)) => {
                if let Some(addr) = info.get_addresses().iter().next().cloned() {
                    let uri = format!("ws://{}:{}", addr, info.get_port());
                    log::info!("mDNS discovered: {}", uri);
                    let _ = tx.send(uri);
                }
            }
            Ok(_) => {}
            Err(e) => {
                log::error!("mDNS browse error: {}", e);
                break;
            }
        }
    });

    std::thread::spawn(move || loop {
        std::thread::sleep(Duration::from_secs(30));
        let _ = daemon.browse(SERVICE_TYPE);
    });

    Ok(rx)
}
