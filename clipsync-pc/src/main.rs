mod clip;
mod config;
mod mdns;
mod protocol;
mod sync;
mod tray;
mod ws;

use std::pin::Pin;
use std::sync::Arc;
use std::time::Duration;
use sync::SyncEngine;
use tokio::sync::mpsc;
use tokio::time::Sleep;
use tray::{ConnState, Tray, TrayAction};
use winit::application::ApplicationHandler;
use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop, EventLoopProxy};

#[derive(Debug)]
enum UiEvent {
    StateChanged(ConnState),
}

struct App {
    tray: Option<Tray>,
    proxy: EventLoopProxy<UiEvent>,
    action_tx: std::sync::mpsc::Sender<TrayAction>,
}

impl App {
    fn new(proxy: EventLoopProxy<UiEvent>, action_tx: std::sync::mpsc::Sender<TrayAction>) -> Self {
        Self {
            tray: None,
            proxy,
            action_tx,
        }
    }
}

impl ApplicationHandler<UiEvent> for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.tray.is_none() {
            match Tray::new(event_loop) {
                Ok(t) => self.tray = Some(t),
                Err(e) => {
                    log::error!("Failed to create tray: {}", e);
                    event_loop.exit();
                }
            }
        }
    }

    fn window_event(
        &mut self,
        _event_loop: &ActiveEventLoop,
        _window_id: winit::window::WindowId,
        _event: winit::event::WindowEvent,
    ) {}

    fn user_event(&mut self, _event_loop: &ActiveEventLoop, event: UiEvent) {
        match event {
            UiEvent::StateChanged(state) => {
                if let Some(ref mut tray) = self.tray {
                    tray.update_state(state);
                }
            }
        }
    }

    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {
        if let Some(ref tray) = self.tray {
            if let Some(action) = tray.try_recv_action() {
                if matches!(action, TrayAction::Quit) {
                    event_loop.exit();
                    return;
                }
                let _ = self.action_tx.send(action);
            }
        }
    }
}

async fn run_sync_loop(
    cfg: Arc<config::ClipSyncConfig>,
    proxy: EventLoopProxy<UiEvent>,
    _action_rx: std::sync::mpsc::Receiver<TrayAction>,
) {
    let (clip_tx, mut clip_rx) = mpsc::unbounded_channel::<String>();
    let listener = clip::ClipListener::new(clip_tx);
    listener.spawn();

    let mut engine = SyncEngine::new();
    let debounce = Duration::from_millis(cfg.clipboard.debounce_ms);
    let mut backoff_secs = 1u64;
    let max_backoff = 60u64;

    let _ = proxy.send_event(UiEvent::StateChanged(ConnState::Disconnected));

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
                _ = tokio::time::sleep(Duration::from_secs(5)) => {}
            }
        };

        let _ = proxy.send_event(UiEvent::StateChanged(ConnState::Connecting));

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

        let _ = proxy.send_event(UiEvent::StateChanged(ConnState::Connected));

        if let Some(pending_text) = engine.take_pending() {
            let msg = protocol::ClipMessage::set(pending_text);
            let _ = ws::send(&mut ws, &msg).await;
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
                        if ws::send(&mut ws, &msg).await.is_err() { break 'connected; }
                        engine.mark_sent(&text);
                    }
                }
                result = ws::recv(&mut ws) => {
                    match result {
                        Ok(protocol::ClipMessage::Push { text, .. }) => {
                            clip::write(&text);
                            engine.mark_sent(&text);
                        }
                        Ok(protocol::ClipMessage::Ping) => {
                            let _ = ws::send(&mut ws, &protocol::ClipMessage::Pong).await;
                        }
                        Ok(protocol::ClipMessage::Pong) => {}
                        Err(_) => break 'connected,
                        _ => {}
                    }
                }
            }
        }

        log::info!("State: Disconnected");
    }
}

fn main() -> anyhow::Result<()> {
    env_logger::init();

    let cfg = Arc::new(config::ClipSyncConfig::load("clipsync.toml")?);
    log::info!("ClipSync PC starting... port={}", cfg.connection.port);

    let event_loop = EventLoop::<UiEvent>::with_user_event().build()?;
    let proxy = event_loop.create_proxy();

    let (action_tx, action_rx) = std::sync::mpsc::channel::<TrayAction>();

    let cfg_clone = cfg.clone();
    let proxy_clone = proxy.clone();
    std::thread::spawn(move || {
        let rt = tokio::runtime::Runtime::new().unwrap();
        rt.block_on(run_sync_loop(cfg_clone, proxy_clone, action_rx));
    });

    let mut app = App::new(proxy, action_tx);
    event_loop.set_control_flow(ControlFlow::Poll);
    event_loop.run_app(&mut app)?;

    Ok(())
}
