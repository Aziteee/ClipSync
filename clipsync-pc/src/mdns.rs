use simple_dns::{rdata::RData, Name, Packet, ResourceRecord};
use socket2::{Domain, Protocol, Socket, Type};
use std::collections::HashMap;
use std::net::{IpAddr, Ipv4Addr, SocketAddrV4, UdpSocket};
use tokio::sync::mpsc as tokio_mpsc;

const SERVICE_TYPE: &str = "_clipsync._tcp.local.";
const MDNS_ADDR: Ipv4Addr = Ipv4Addr::new(224, 0, 0, 251);
const MDNS_PORT: u16 = 5353;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiscoveredEndpoint {
    pub uri: String,
    pub name: String,
    pub service_name: String,
}

pub fn discover(_port: u16) -> anyhow::Result<tokio_mpsc::UnboundedReceiver<DiscoveredEndpoint>> {
    let (tx, rx) = tokio_mpsc::unbounded_channel();
    start_passive_announce_listener(tx.clone())?;

    Ok(rx)
}

fn start_passive_announce_listener(
    tx: tokio_mpsc::UnboundedSender<DiscoveredEndpoint>,
) -> anyhow::Result<()> {
    let socket = bind_mdns_socket()?;
    std::thread::spawn(move || {
        let mut buf = [0u8; 2048];
        while !tx.is_closed() {
            match socket.recv_from(&mut buf) {
                Ok((len, _from)) => {
                    for endpoint in parse_announce_packet(&buf[..len]) {
                        log::info!(
                            "mDNS announce discovered: {} service={}",
                            endpoint.uri,
                            endpoint.service_name
                        );
                        let _ = tx.send(endpoint);
                    }
                }
                Err(e) => {
                    log::warn!("Passive mDNS receive failed: {}", e);
                }
            }
        }
    });
    Ok(())
}

fn bind_mdns_socket() -> anyhow::Result<UdpSocket> {
    let socket = Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP))?;
    socket.set_reuse_address(true)?;
    #[cfg(unix)]
    socket.set_reuse_port(true)?;
    socket.bind(&SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, MDNS_PORT).into())?;
    let socket: UdpSocket = socket.into();
    socket.join_multicast_v4(&MDNS_ADDR, &Ipv4Addr::UNSPECIFIED)?;
    Ok(socket)
}

fn instance_name(fullname: &str) -> &str {
    let instance = fullname
        .strip_suffix(SERVICE_TYPE)
        .and_then(|s| s.strip_suffix('.'))
        .filter(|s| !s.is_empty())
        .unwrap_or(fullname);
    instance
        .strip_prefix("ClipSync-Android-")
        .filter(|s| !s.is_empty())
        .unwrap_or(instance)
}

fn ws_uri(addr: IpAddr, port: u16) -> String {
    match addr {
        IpAddr::V4(addr) => format!("ws://{}:{}/ws", addr, port),
        IpAddr::V6(addr) => format!("ws://[{}]:{}/ws", addr, port),
    }
}

#[derive(Default)]
struct PacketRecords {
    ptr_instances: Vec<String>,
    srv: HashMap<String, (u16, String)>,
    a: HashMap<String, Ipv4Addr>,
}

fn parse_announce_packet(packet: &[u8]) -> Vec<DiscoveredEndpoint> {
    let Some(records) = parse_dns_records(packet) else {
        return Vec::new();
    };

    let mut endpoints = Vec::new();
    for service_name in records.ptr_instances {
        let Some((port, host)) = records.srv.get(&service_name) else {
            continue;
        };
        let Some(addr) = records.a.get(host) else {
            continue;
        };
        endpoints.push(DiscoveredEndpoint {
            uri: ws_uri(IpAddr::V4(*addr), *port),
            name: instance_name(&service_name).to_string(),
            service_name,
        });
    }
    endpoints
}

fn parse_dns_records(packet: &[u8]) -> Option<PacketRecords> {
    let packet = Packet::parse(packet).ok()?;
    let mut records = PacketRecords::default();
    for rr in packet
        .answers
        .iter()
        .chain(packet.name_servers.iter())
        .chain(packet.additional_records.iter())
    {
        collect_record(rr, &mut records);
    }

    Some(records)
}

fn collect_record(rr: &ResourceRecord<'_>, records: &mut PacketRecords) {
    let name = normalize_name(&rr.name);
    match &rr.rdata {
        RData::A(addr) => {
            records.a.insert(name, Ipv4Addr::from(addr.address));
        }
        RData::PTR(ptr) if same_dns_name(&name, SERVICE_TYPE) => {
            records.ptr_instances.push(normalize_name(ptr));
        }
        RData::SRV(srv) => {
            records
                .srv
                .insert(name, (srv.port, normalize_name(&srv.target)));
        }
        _ => {}
    }
}

fn normalize_name(name: &Name<'_>) -> String {
    let name = name.to_string();
    if name == "." || name.ends_with('.') {
        name
    } else {
        format!("{}.", name)
    }
}

fn normalize_name_str(name: &str) -> String {
    if name == "." || name.ends_with('.') {
        name.to_string()
    } else {
        format!("{}.", name)
    }
}

fn same_dns_name(a: &str, b: &str) -> bool {
    normalize_name_str(a).eq_ignore_ascii_case(&normalize_name_str(b))
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
    fn instance_name_strips_service_suffix() {
        assert_eq!(
            instance_name("ClipSync-Android-ABC123._clipsync._tcp.local."),
            "ABC123"
        );
        assert_eq!(instance_name("Phone"), "Phone");
    }

    #[test]
    fn passive_parser_reads_ptr_srv_and_a_records() {
        let packet = sample_announcement_packet();
        let endpoints = parse_announce_packet(&packet);

        assert_eq!(endpoints.len(), 1);
        assert_eq!(endpoints[0].uri, "ws://192.168.0.159:5287/ws");
        assert_eq!(endpoints[0].name, "f2d65699");
        assert_eq!(
            endpoints[0].service_name,
            "ClipSync-Android-f2d65699._clipsync._tcp.local."
        );
    }

    fn sample_announcement_packet() -> Vec<u8> {
        let mut packet = Vec::new();
        packet.extend_from_slice(&[
            0x00, 0x00, 0x84, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
        ]);

        let service_offset = packet.len();
        push_name(&mut packet, &["_clipsync", "_tcp", "local"]);
        push_rr_fixed(&mut packet, 12, 1, 120);
        let ptr_len_pos = reserve_rdlen(&mut packet);
        push_name(
            &mut packet,
            &["ClipSync-Android-f2d65699", "_clipsync", "_tcp", "local"],
        );
        fill_rdlen(&mut packet, ptr_len_pos);

        let instance_offset = ptr_len_pos + 2;
        push_compressed_name(&mut packet, instance_offset);
        push_rr_fixed(&mut packet, 33, 1, 120);
        let srv_len_pos = reserve_rdlen(&mut packet);
        packet.extend_from_slice(&[0, 0, 0, 0, 0x14, 0xa7]);
        push_name(&mut packet, &["ClipSync-Android-f2d65699", "local"]);
        fill_rdlen(&mut packet, srv_len_pos);

        push_name(&mut packet, &["ClipSync-Android-f2d65699", "local"]);
        push_rr_fixed(&mut packet, 1, 1, 120);
        packet.extend_from_slice(&[0, 4, 192, 168, 0, 159]);

        assert!(service_offset < 0x3fff);
        packet
    }

    fn push_name(packet: &mut Vec<u8>, labels: &[&str]) {
        for label in labels {
            packet.push(label.len() as u8);
            packet.extend_from_slice(label.as_bytes());
        }
        packet.push(0);
    }

    fn push_compressed_name(packet: &mut Vec<u8>, offset: usize) {
        packet.push(0xc0 | ((offset >> 8) as u8 & 0x3f));
        packet.push((offset & 0xff) as u8);
    }

    fn push_rr_fixed(packet: &mut Vec<u8>, rr_type: u16, class: u16, ttl: u32) {
        packet.extend_from_slice(&rr_type.to_be_bytes());
        packet.extend_from_slice(&class.to_be_bytes());
        packet.extend_from_slice(&ttl.to_be_bytes());
    }

    fn reserve_rdlen(packet: &mut Vec<u8>) -> usize {
        let pos = packet.len();
        packet.extend_from_slice(&[0, 0]);
        pos
    }

    fn fill_rdlen(packet: &mut [u8], pos: usize) {
        let len = (packet.len() - pos - 2) as u16;
        packet[pos..pos + 2].copy_from_slice(&len.to_be_bytes());
    }
}
