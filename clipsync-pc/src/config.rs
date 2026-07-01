use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClipSyncConfig {
    #[serde(default)]
    pub connection: ConnectionConfig,
    #[serde(default)]
    pub auth: AuthConfig,
    #[serde(default)]
    pub clipboard: ClipboardConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ConnectionConfig {
    #[serde(default = "default_port")]
    pub port: u16,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AuthConfig {
    #[serde(default)]
    pub secret: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClipboardConfig {
    #[serde(default = "default_debounce_ms")]
    pub debounce_ms: u64,
}

fn default_port() -> u16 { 5287 }
fn default_debounce_ms() -> u64 { 300 }

impl Default for ClipSyncConfig {
    fn default() -> Self {
        Self {
            connection: ConnectionConfig::default(),
            auth: AuthConfig::default(),
            clipboard: ClipboardConfig::default(),
        }
    }
}

impl Default for ConnectionConfig {
    fn default() -> Self {
        Self { port: default_port() }
    }
}

impl Default for AuthConfig {
    fn default() -> Self {
        Self { secret: String::new() }
    }
}

impl Default for ClipboardConfig {
    fn default() -> Self {
        Self { debounce_ms: default_debounce_ms() }
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
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_default_config() {
        let cfg = ClipSyncConfig::default();
        assert_eq!(cfg.connection.port, 5287);
        assert_eq!(cfg.clipboard.debounce_ms, 300);
        assert!(cfg.auth.secret.is_empty());
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
    fn test_partial_config() {
        let toml_str = r#"
[auth]
secret = "only-secret"
"#;
        let cfg: ClipSyncConfig = toml::from_str(toml_str).unwrap();
        assert_eq!(cfg.connection.port, 5287);
        assert_eq!(cfg.clipboard.debounce_ms, 300);
    }
}
