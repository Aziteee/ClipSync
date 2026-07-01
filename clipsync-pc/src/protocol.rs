use serde::{Deserialize, Serialize};
use std::time::SystemTime;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum ClipMessage {
    #[serde(rename = "clipboard_push")]
    Push { text: String, ts: u64 },
    #[serde(rename = "clipboard_set")]
    Set { text: String, ts: u64 },
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

    pub fn push(text: String) -> Self {
        ClipMessage::Push { text, ts: now_millis() }
    }

    pub fn set(text: String) -> Self {
        ClipMessage::Set { text, ts: now_millis() }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_push_roundtrip() {
        let msg = ClipMessage::Push { text: "hello".into(), ts: 1719859200 };
        let json = msg.to_json();
        let decoded = ClipMessage::from_json(&json).unwrap();
        match decoded {
            ClipMessage::Push { text, ts } => {
                assert_eq!(text, "hello");
                assert_eq!(ts, 1719859200);
            }
            _ => panic!("wrong variant"),
        }
    }

    #[test]
    fn test_hello_roundtrip() {
        let msg = ClipMessage::Hello { challenge: "abc123".into() };
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
        let msg = ClipMessage::Push { text: "test".into(), ts: 123 };
        let json = msg.to_json();
        assert!(json.contains("\"type\":\"clipboard_push\""));
        assert!(json.contains("\"text\":\"test\""));
    }
}
