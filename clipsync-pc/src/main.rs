mod clip;
mod config;
mod mdns;
mod protocol;
mod sync;
mod ws;

use std::pin::Pin;
use std::time::Duration;
use sync::SyncEngine;
use tokio::sync::mpsc;
use tokio::time::Sleep;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    env_logger::init();

    let cfg = config::ClipSyncConfig::load("clipsync.toml")?;
    log::info!("ClipSync PC starting... port={}", cfg.connection.port);

    let (clip_tx, mut clip_rx) = mpsc::unbounded_channel::<String>();
    let listener = clip::ClipListener::new(clip_tx);
    listener.spawn();

    let mut engine = SyncEngine::new();
    let debounce = Duration::from_millis(cfg.clipboard.debounce_ms);
    let mut backoff_secs = 1u64;
    let max_backoff = 60u64;

    loop {
        log::info!("State: Disconnected — starting mDNS discovery");
        let Ok(mut mdns_rx) = mdns::discover() else {
            log::error!("mDNS discovery failed, retrying in 5s");
            tokio::time::sleep(Duration::from_secs(5)).await;
            continue;
        };

        let uri = loop {
            tokio::select! {
                Some(uri) = mdns_rx.recv() => break uri,
                Some(text) = clip_rx.recv() => {
                    engine.store_pending(text);
                }
                _ = tokio::time::sleep(Duration::from_secs(5)) => {
                    log::debug!("Waiting for mDNS discovery...");
                }
            }
        };

        let mut ws = loop {
            log::info!("Connecting to {}...", uri);
            match ws::connect_and_auth(&uri, &cfg.auth.secret).await {
                Ok(ws) => {
                    log::info!("Connected and authenticated!");
                    break ws;
                }
                Err(e) => {
                    log::error!("Connection failed: {}", e);
                    log::info!("Retrying in {}s...", backoff_secs);
                    tokio::time::sleep(Duration::from_secs(backoff_secs)).await;
                    backoff_secs = (backoff_secs * 2).min(max_backoff);
                }
            }
        };
        backoff_secs = 1;

        log::info!("State: Connected");

        if let Some(pending_text) = engine.take_pending() {
            let msg = protocol::ClipMessage::set(pending_text);
            if let Err(e) = ws::send(&mut ws, &msg).await {
                log::error!("Failed to flush pending: {}", e);
            } else {
                log::info!("Flushed pending clipboard to phone");
            }
        }

        let mut debounce_text: Option<String> = None;
        let mut debounce_sleep: Pin<Box<Sleep>> = Box::pin(tokio::time::sleep(Duration::MAX));

        'connected: loop {
            tokio::select! {
                Some(text) = clip_rx.recv() => {
                    debounce_text = Some(text);
                    debounce_sleep.as_mut().reset(tokio::time::Instant::now() + debounce);
                }
                _ = &mut debounce_sleep, if debounce_text.is_some() => {
                    let text = debounce_text.take().unwrap();
                    if engine.should_send(&text) {
                        let msg = protocol::ClipMessage::set(text.clone());
                        if ws::send(&mut ws, &msg).await.is_err() {
                            break 'connected;
                        }
                        engine.mark_sent(&text);
                        log::info!("Sent to phone: {} chars", text.len());
                    }
                }
                result = ws::recv(&mut ws) => {
                    match result {
                        Ok(protocol::ClipMessage::Push { text, .. }) => {
                            log::info!("Received from phone: {} chars", text.len());
                            clip::write(&text);
                            engine.mark_sent(&text);
                        }
                        Ok(protocol::ClipMessage::Ping) => {
                            let _ = ws::send(&mut ws, &protocol::ClipMessage::Pong).await;
                        }
                        Ok(protocol::ClipMessage::Pong) => {}
                        Err(e) => {
                            log::error!("Connection lost: {}", e);
                            break 'connected;
                        }
                        _ => {}
                    }
                }
            }
        }

        log::info!("State: Disconnected");
    }
}
