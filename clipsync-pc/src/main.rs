mod clip;
mod config;
mod mdns;
mod protocol;
mod sync;
mod ws;

use std::time::Duration;
use sync::SyncEngine;
use tokio::sync::mpsc;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    #[cfg(debug_assertions)]
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    #[cfg(not(debug_assertions))]
    env_logger::init();

    let cfg = config::ClipSyncConfig::load("clipsync.toml")?;
    log::info!(
        "ClipSync PC starting... port={} debounce={}ms",
        cfg.connection.port,
        cfg.clipboard.debounce_ms
    );

    let (clip_tx, mut clip_rx) = mpsc::unbounded_channel::<String>();
    let listener = clip::ClipListener::new(clip_tx);
    listener.spawn();

    let mut engine = SyncEngine::new();
    let debounce = Duration::from_millis(cfg.clipboard.debounce_ms);

    log::info!("Listening for clipboard changes...");

    loop {
        let Some(mut text) = clip_rx.recv().await else { break };

        loop {
            tokio::select! {
                _ = tokio::time::sleep(debounce) => { break; }
                Some(new_text) = clip_rx.recv() => { text = new_text; }
            }
        }

        if engine.should_send(&text) {
            log::info!("Clipboard changed: {} chars", text.len());
            engine.mark_sent(&text);
            let msg = protocol::ClipMessage::push(text);
            log::info!("Would send: {}", msg.to_json());
        }
    }

    Ok(())
}
