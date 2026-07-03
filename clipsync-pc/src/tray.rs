use crate::UiEvent;
use tray_icon::{
    menu::{CheckMenuItem, Menu, MenuEvent, MenuItem, PredefinedMenuItem},
    Icon, TrayIcon, TrayIconBuilder,
};
use winit::event_loop::{ActiveEventLoop, EventLoopProxy};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnState {
    Disconnected,
    Connecting,
    Connected,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TrayAction {
    Reconnect,
    TogglePause,
    ToggleStartWithWindows,
    Quit,
}

pub struct Tray {
    tray_icon: TrayIcon,
    _menu: Menu,
    autostart_item: CheckMenuItem,
    conn_state: ConnState,
    paused: bool,
    start_with_windows: bool,
}

fn make_icon_data(r: u8, g: u8, b: u8) -> Vec<u8> {
    let size: u32 = 32;
    let radius: f64 = 14.0;
    let center = size as f64 / 2.0;
    let mut data = Vec::with_capacity((size * size * 4) as usize);
    for y in 0..size {
        for x in 0..size {
            let dx = x as f64 - center;
            let dy = y as f64 - center;
            let dist = (dx * dx + dy * dy).sqrt();
            if dist <= radius {
                data.extend_from_slice(&[r, g, b, 255]);
            } else {
                data.extend_from_slice(&[0, 0, 0, 0]);
            }
        }
    }
    data
}

fn icon_for_state(state: ConnState) -> Icon {
    let (r, g, b) = match state {
        ConnState::Connected => (0, 200, 0),
        ConnState::Connecting => (200, 200, 0),
        ConnState::Disconnected => (128, 128, 128),
    };
    let rgba = make_icon_data(r, g, b);
    Icon::from_rgba(rgba, 32, 32).expect("failed to create icon")
}

impl Tray {
    pub fn new(
        event_loop: &ActiveEventLoop,
        proxy: EventLoopProxy<UiEvent>,
        start_with_windows: bool,
    ) -> anyhow::Result<Self> {
        let menu = Menu::new();
        let reconnect_item = MenuItem::new("Reconnect", true, None);
        let pause_item = MenuItem::new("Pause Sync", true, None);
        let autostart_item =
            CheckMenuItem::new("Start with Windows", true, start_with_windows, None);
        let separator = PredefinedMenuItem::separator();
        let quit_item = MenuItem::new("Quit", true, None);

        menu.append(&reconnect_item)?;
        menu.append(&pause_item)?;
        menu.append(&autostart_item)?;
        menu.append(&separator)?;
        menu.append(&quit_item)?;

        let icon = icon_for_state(ConnState::Disconnected);
        let tray_icon = TrayIconBuilder::new()
            .with_menu(Box::new(menu.clone()))
            .with_icon(icon)
            .with_tooltip("ClipSync \u{b7} Not connected")
            .build()?;

        let reconnect_id = reconnect_item.id().clone();
        let pause_id = pause_item.id().clone();
        let autostart_id = autostart_item.id().clone();
        let quit_id = quit_item.id().clone();

        std::thread::spawn(move || loop {
            if let Ok(event) = MenuEvent::receiver().recv() {
                let action = if event.id == reconnect_id {
                    Some(TrayAction::Reconnect)
                } else if event.id == pause_id {
                    Some(TrayAction::TogglePause)
                } else if event.id == autostart_id {
                    Some(TrayAction::ToggleStartWithWindows)
                } else if event.id == quit_id {
                    Some(TrayAction::Quit)
                } else {
                    None
                };
                if let Some(action) = action {
                    let _ = proxy.send_event(UiEvent::TrayAction(action));
                }
            }
        });

        let _ = event_loop;

        Ok(Self {
            tray_icon,
            _menu: menu,
            autostart_item,
            conn_state: ConnState::Disconnected,
            paused: false,
            start_with_windows,
        })
    }

    pub fn update_state(&mut self, state: ConnState) {
        self.conn_state = state;
        self.refresh();
    }

    pub fn set_paused(&mut self, paused: bool) {
        self.paused = paused;
        self.refresh();
    }

    pub fn set_start_with_windows(&mut self, enabled: bool) {
        self.start_with_windows = enabled;
        self.autostart_item.set_checked(enabled);
    }

    fn refresh(&mut self) {
        if self.paused {
            let rgba = make_icon_data(255, 165, 0);
            if let Ok(icon) = Icon::from_rgba(rgba, 32, 32) {
                let _ = self.tray_icon.set_icon(Some(icon));
            }
            let _ = self.tray_icon.set_tooltip(Some("ClipSync \u{b7} Paused"));
        } else {
            let icon = icon_for_state(self.conn_state);
            let tooltip = match self.conn_state {
                ConnState::Connected => "ClipSync \u{b7} Connected",
                ConnState::Connecting => "ClipSync \u{b7} Connecting\u{2026}",
                ConnState::Disconnected => "ClipSync \u{b7} Not connected",
            };
            let _ = self.tray_icon.set_icon(Some(icon));
            let _ = self.tray_icon.set_tooltip(Some(tooltip));
        }
    }
}
