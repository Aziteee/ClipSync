use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct GeneralConfig {
    #[serde(default)]
    pub start_with_windows: bool,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ClipSyncConfig {
    #[serde(default)]
    pub connection: ConnectionConfig,
    #[serde(default)]
    pub auth: AuthConfig,
    #[serde(default)]
    pub clipboard: ClipboardConfig,
    #[serde(default)]
    pub general: GeneralConfig,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub devices: Vec<DeviceConfig>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ConnectionConfig {
    #[serde(default = "default_port")]
    pub port: u16,
    #[serde(default)]
    pub host: Option<String>,
    #[serde(default)]
    pub uri: Option<String>,
    #[serde(default = "default_heartbeat_interval_ms")]
    pub heartbeat_interval_ms: u64,
    #[serde(default = "default_heartbeat_timeout_ms")]
    pub heartbeat_timeout_ms: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct AuthConfig {
    #[serde(default)]
    pub secret: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClipboardConfig {
    #[serde(default = "default_debounce_ms")]
    pub debounce_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceConfig {
    #[serde(default)]
    pub name: Option<String>,
    #[serde(default)]
    pub uri: String,
    #[serde(default = "default_device_enabled")]
    pub enabled: bool,
}

fn default_port() -> u16 {
    5287
}
fn default_heartbeat_interval_ms() -> u64 {
    15_000
}
fn default_heartbeat_timeout_ms() -> u64 {
    45_000
}
fn default_debounce_ms() -> u64 {
    300
}
fn default_device_enabled() -> bool {
    true
}

impl Default for ConnectionConfig {
    fn default() -> Self {
        Self {
            port: default_port(),
            host: None,
            uri: None,
            heartbeat_interval_ms: default_heartbeat_interval_ms(),
            heartbeat_timeout_ms: default_heartbeat_timeout_ms(),
        }
    }
}

impl Default for ClipboardConfig {
    fn default() -> Self {
        Self {
            debounce_ms: default_debounce_ms(),
        }
    }
}

impl ClipSyncConfig {
    pub fn load(path: impl AsRef<Path>) -> anyhow::Result<Self> {
        let path = path.as_ref();
        if path.exists() {
            let content = std::fs::read_to_string(path)?;
            Ok(toml::from_str(&content)?)
        } else {
            Ok(Self::default())
        }
    }

    pub fn save(&self, path: impl AsRef<Path>) -> anyhow::Result<()> {
        let content = toml::to_string_pretty(self)?;
        std::fs::write(path, content)?;
        Ok(())
    }

    pub fn has_empty_secret(&self) -> bool {
        self.auth.secret.trim().is_empty()
    }
}

impl ConnectionConfig {
    pub fn direct_ws_uri(&self) -> Option<String> {
        if let Some(uri) = non_empty(&self.uri) {
            return Some(uri.to_string());
        }

        let host = non_empty(&self.host)?;
        Some(format!("ws://{}:{}/ws", format_host(host), self.port))
    }
}

fn non_empty(value: &Option<String>) -> Option<&str> {
    value.as_deref().map(str::trim).filter(|s| !s.is_empty())
}

fn format_host(host: &str) -> String {
    if host.contains(':') && !host.starts_with('[') {
        format!("[{}]", host)
    } else {
        host.to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_default_config() {
        let cfg = ClipSyncConfig::default();
        assert_eq!(cfg.connection.port, 5287);
        assert_eq!(cfg.clipboard.debounce_ms, 300);
        assert_eq!(cfg.connection.heartbeat_interval_ms, 15_000);
        assert_eq!(cfg.connection.heartbeat_timeout_ms, 45_000);
        assert!(cfg.auth.secret.is_empty());
        assert!(!cfg.general.start_with_windows);
    }

    #[test]
    fn test_empty_secret_is_detected() {
        let cfg = ClipSyncConfig::default();
        assert!(cfg.has_empty_secret());
    }

    #[test]
    fn test_non_empty_secret_is_not_detected_as_empty() {
        let mut cfg = ClipSyncConfig::default();
        cfg.auth.secret = "test-key".into();
        assert!(!cfg.has_empty_secret());
    }

    #[test]
    fn test_parse_toml() {
        let toml_str = r#"
[connection]
port = 9999

[auth]
secret = "test-key"

[clipboard]
debounce_ms = 500
"#;
        let cfg: ClipSyncConfig = toml::from_str(toml_str).unwrap();
        assert_eq!(cfg.connection.port, 9999);
        assert_eq!(cfg.auth.secret, "test-key");
        assert_eq!(cfg.clipboard.debounce_ms, 500);
    }

    #[test]
    fn test_parse_static_devices() {
        let toml_str = r#"
[[devices]]
name = "Phone"
uri = "ws://192.168.0.10:5287/ws"

[[devices]]
name = "Tablet"
uri = "ws://192.168.0.11:5287/ws"
enabled = false
"#;
        let cfg: ClipSyncConfig = toml::from_str(toml_str).unwrap();
        assert_eq!(cfg.devices.len(), 2);
        assert_eq!(cfg.devices[0].name.as_deref(), Some("Phone"));
        assert_eq!(cfg.devices[0].uri, "ws://192.168.0.10:5287/ws");
        assert!(cfg.devices[0].enabled);
        assert!(!cfg.devices[1].enabled);
    }

    #[test]
    fn test_partial_config() {
        let toml_str = r#"
[auth]
secret = "only-secret"
"#;
        let cfg: ClipSyncConfig = toml::from_str(toml_str).unwrap();
        assert_eq!(cfg.connection.port, 5287);
        assert_eq!(cfg.clipboard.debounce_ms, 300);
    }

    #[test]
    fn test_direct_host_builds_ws_uri() {
        let toml_str = r#"
[connection]
host = "192.168.0.103"
"#;
        let cfg: ClipSyncConfig = toml::from_str(toml_str).unwrap();
        assert_eq!(
            cfg.connection.direct_ws_uri().as_deref(),
            Some("ws://192.168.0.103:5287/ws")
        );
    }

    #[test]
    fn test_direct_uri_overrides_host() {
        let toml_str = r#"
[connection]
host = "192.168.0.103"
uri = "ws://10.0.0.5:5287/ws"
"#;
        let cfg: ClipSyncConfig = toml::from_str(toml_str).unwrap();
        assert_eq!(
            cfg.connection.direct_ws_uri().as_deref(),
            Some("ws://10.0.0.5:5287/ws")
        );
    }
}
