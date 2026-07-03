use blake3::Hash;

pub struct SyncEngine {
    last_sent_hash: Option<Hash>,
    pending: Option<String>,
}

impl SyncEngine {
    pub fn new() -> Self {
        Self {
            last_sent_hash: None,
            pending: None,
        }
    }

    fn hash(text: &str) -> Hash {
        blake3::hash(text.as_bytes())
    }

    pub fn should_send(&self, text: &str) -> bool {
        let h = Self::hash(text);
        self.last_sent_hash != Some(h)
    }

    pub fn mark_sent(&mut self, text: &str) {
        self.last_sent_hash = Some(Self::hash(text));
    }

    pub fn store_pending(&mut self, text: String) {
        self.pending = Some(text);
    }

    pub fn take_pending(&mut self) -> Option<String> {
        self.pending.take()
    }

    #[cfg(test)]
    pub fn has_pending(&self) -> bool {
        self.pending.is_some()
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
        assert!(engine.should_send("hello"));
    }

    #[test]
    fn test_duplicate_is_skipped() {
        let mut engine = SyncEngine::new();
        engine.mark_sent("hello");
        assert!(!engine.should_send("hello"));
    }

    #[test]
    fn test_different_text_sends() {
        let mut engine = SyncEngine::new();
        engine.mark_sent("hello");
        assert!(engine.should_send("world"));
    }

    #[test]
    fn test_pending_empty_by_default() {
        let engine = SyncEngine::new();
        assert!(!engine.has_pending());
    }

    #[test]
    fn test_pending_store_and_take() {
        let mut engine = SyncEngine::new();
        engine.store_pending("offline text".into());
        assert!(engine.has_pending());
        assert_eq!(engine.take_pending(), Some("offline text".into()));
        assert!(!engine.has_pending());
        assert!(engine.take_pending().is_none());
    }

    #[test]
    fn test_pending_only_keeps_latest() {
        let mut engine = SyncEngine::new();
        engine.store_pending("first".into());
        engine.store_pending("second".into());
        assert_eq!(engine.take_pending(), Some("second".into()));
    }
}
