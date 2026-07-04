use blake3::Hash;
use std::collections::HashMap;

pub struct SyncEngine {
    last_broadcast_hash: Option<Hash>,
    pending_by_device: HashMap<String, String>,
}

impl SyncEngine {
    pub fn new() -> Self {
        Self {
            last_broadcast_hash: None,
            pending_by_device: HashMap::new(),
        }
    }

    fn hash(text: &str) -> Hash {
        blake3::hash(text.as_bytes())
    }

    pub fn should_broadcast(&self, text: &str) -> bool {
        let h = Self::hash(text);
        self.last_broadcast_hash != Some(h)
    }

    pub fn mark_broadcast(&mut self, text: &str) {
        self.last_broadcast_hash = Some(Self::hash(text));
    }

    pub fn store_pending_for(&mut self, device_id: impl Into<String>, text: String) {
        self.pending_by_device.insert(device_id.into(), text);
    }

    pub fn take_pending_for(&mut self, device_id: &str) -> Option<String> {
        self.pending_by_device.remove(device_id)
    }

    #[cfg(test)]
    pub fn has_pending_for(&self, device_id: &str) -> bool {
        self.pending_by_device.contains_key(device_id)
    }
}

impl Default for SyncEngine {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_first_text_always_sends() {
        let engine = SyncEngine::new();
        assert!(engine.should_broadcast("hello"));
    }

    #[test]
    fn test_duplicate_is_skipped() {
        let mut engine = SyncEngine::new();
        engine.mark_broadcast("hello");
        assert!(!engine.should_broadcast("hello"));
    }

    #[test]
    fn test_different_text_sends() {
        let mut engine = SyncEngine::new();
        engine.mark_broadcast("hello");
        assert!(engine.should_broadcast("world"));
    }

    #[test]
    fn test_pending_empty_by_default() {
        let engine = SyncEngine::new();
        assert!(!engine.has_pending_for("phone"));
    }

    #[test]
    fn test_pending_store_and_take() {
        let mut engine = SyncEngine::new();
        engine.store_pending_for("phone", "offline text".into());
        assert!(engine.has_pending_for("phone"));
        assert_eq!(
            engine.take_pending_for("phone"),
            Some("offline text".into())
        );
        assert!(!engine.has_pending_for("phone"));
        assert!(engine.take_pending_for("phone").is_none());
    }

    #[test]
    fn test_pending_only_keeps_latest() {
        let mut engine = SyncEngine::new();
        engine.store_pending_for("phone", "first".into());
        engine.store_pending_for("phone", "second".into());
        assert_eq!(engine.take_pending_for("phone"), Some("second".into()));
    }

    #[test]
    fn test_pending_is_per_device() {
        let mut engine = SyncEngine::new();
        engine.store_pending_for("phone", "phone text".into());
        engine.store_pending_for("tablet", "tablet text".into());
        assert_eq!(engine.take_pending_for("phone"), Some("phone text".into()));
        assert_eq!(
            engine.take_pending_for("tablet"),
            Some("tablet text".into())
        );
    }
}
