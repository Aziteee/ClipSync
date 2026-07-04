use serde::{Deserialize, Serialize};
use std::time::SystemTime;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum ClipMessage {
    #[serde(rename = "clipboard_push")]
    Push {
        text: String,
        ts: u64,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        event_id: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        origin: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        device_id: Option<String>,
    },
    #[serde(rename = "clipboard_set")]
    Set {
        text: String,
        ts: u64,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        event_id: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        origin: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        device_id: Option<String>,
    },
    #[serde(rename = "ping")]
    Ping,
    #[serde(rename = "pong")]
    Pong,
    #[serde(rename = "hello")]
    Hello { challenge: String },
    #[serde(rename = "auth")]
    Auth { response: String },
    #[serde(rename = "auth_ok")]
    AuthOk,
    #[serde(rename = "auth_fail")]
    AuthFail,
}

pub fn now_millis() -> u64 {
    SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

impl ClipMessage {
    pub fn to_json(&self) -> String {
        serde_json::to_string(self).unwrap()
    }

    pub fn from_json(json: &str) -> serde_json::Result<Self> {
        serde_json::from_str(json)
    }

    #[cfg(test)]
    fn push(text: String) -> Self {
        ClipMessage::Push {
            text,
            ts: now_millis(),
            event_id: None,
            origin: None,
            device_id: None,
        }
    }

    #[allow(dead_code)]
    pub fn set(text: String) -> Self {
        Self::set_with_meta(text, None, None, None)
    }

    pub fn set_with_meta(
        text: String,
        event_id: Option<String>,
        origin: Option<String>,
        device_id: Option<String>,
    ) -> Self {
        ClipMessage::Set {
            text,
            ts: now_millis(),
            event_id,
            origin,
            device_id,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_push_roundtrip() {
        let msg = ClipMessage::push("hello".into());
        let json = msg.to_json();
        let decoded = ClipMessage::from_json(&json).unwrap();
        match decoded {
            ClipMessage::Push { text, .. } => {
                assert_eq!(text, "hello");
            }
            _ => panic!("wrong variant"),
        }
    }

    #[test]
    fn test_hello_roundtrip() {
        let msg = ClipMessage::Hello {
            challenge: "abc123".into(),
        };
        let json = msg.to_json();
        let decoded = ClipMessage::from_json(&json).unwrap();
        match decoded {
            ClipMessage::Hello { challenge } => {
                assert_eq!(challenge, "abc123");
            }
            _ => panic!("wrong variant"),
        }
    }

    #[test]
    fn test_ping_roundtrip() {
        let msg = ClipMessage::Ping;
        let json = msg.to_json();
        let decoded = ClipMessage::from_json(&json).unwrap();
        assert!(matches!(decoded, ClipMessage::Ping));
    }

    #[test]
    fn test_push_json_format() {
        let msg = ClipMessage::Push {
            text: "test".into(),
            ts: 123,
            event_id: Some("evt-1".into()),
            origin: Some("pc".into()),
            device_id: Some("android".into()),
        };
        let json = msg.to_json();
        assert!(json.contains("\"type\":\"clipboard_push\""));
        assert!(json.contains("\"text\":\"test\""));
        assert!(json.contains("\"event_id\":\"evt-1\""));
    }

    #[test]
    fn test_old_push_json_without_optional_metadata_still_decodes() {
        let decoded =
            ClipMessage::from_json(r#"{"type":"clipboard_push","text":"hello","ts":123}"#).unwrap();
        match decoded {
            ClipMessage::Push {
                text,
                event_id,
                origin,
                device_id,
                ..
            } => {
                assert_eq!(text, "hello");
                assert_eq!(event_id, None);
                assert_eq!(origin, None);
                assert_eq!(device_id, None);
            }
            _ => panic!("wrong variant"),
        }
    }
}
