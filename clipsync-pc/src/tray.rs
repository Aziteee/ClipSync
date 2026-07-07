use crate::UiEvent;
use tray_icon::{
    menu::{CheckMenuItem, Menu, MenuEvent, MenuItem, PredefinedMenuItem, Submenu},
    Icon, TrayIcon, TrayIconBuilder,
};
use winit::event_loop::{ActiveEventLoop, EventLoopProxy};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConnState {
    Disconnected,
    Connecting { devices: Vec<DeviceSummary> },
    Connected { devices: Vec<DeviceSummary> },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DeviceSummary {
    pub name: Option<String>,
    pub ip: String,
    pub state: DeviceSummaryState,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeviceSummaryState {
    Disconnected,
    Connecting,
    Connected,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TrayAction {
    ToggleStartWithWindows,
    CheckForUpdates,
    Quit,
}

pub struct Tray {
    tray_icon: TrayIcon,
    _menu: Menu,
    devices_submenu: Submenu,
    device_items: Vec<MenuItem>,
    autostart_item: CheckMenuItem,
    conn_state: ConnState,
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
        ConnState::Connected { .. } => (0, 200, 0),
        ConnState::Connecting { .. } => (200, 200, 0),
        ConnState::Disconnected => (128, 128, 128),
    };
    let rgba = make_icon_data(r, g, b);
    Icon::from_rgba(rgba, 32, 32).expect("failed to create icon")
}

impl Tray {
    pub fn new(
        _event_loop: &ActiveEventLoop,
        proxy: EventLoopProxy<UiEvent>,
        start_with_windows: bool,
    ) -> anyhow::Result<Self> {
        let menu = Menu::new();
        let devices_submenu = Submenu::new("Devices: 0 connected", true);
        let device_separator = PredefinedMenuItem::separator();
        let autostart_item =
            CheckMenuItem::new("Start with Windows", true, start_with_windows, None);
        let separator = PredefinedMenuItem::separator();
        let update_item = MenuItem::new("Check for Updates", true, None);
        let quit_item = MenuItem::new("Quit", true, None);

        menu.append(&devices_submenu)?;
        menu.append(&device_separator)?;
        menu.append(&autostart_item)?;
        menu.append(&separator)?;
        menu.append(&update_item)?;
        menu.append(&quit_item)?;

        let icon = icon_for_state(ConnState::Disconnected);
        let tray_icon = TrayIconBuilder::new()
            .with_menu(Box::new(menu.clone()))
            .with_icon(icon)
            .with_tooltip("ClipSync \u{b7} Not connected")
            .build()?;

        let autostart_id = autostart_item.id().clone();
        let update_id = update_item.id().clone();
        let quit_id = quit_item.id().clone();

        std::thread::spawn(move || loop {
            if let Ok(event) = MenuEvent::receiver().recv() {
                let action = if event.id == autostart_id {
                    Some(TrayAction::ToggleStartWithWindows)
                } else if event.id == update_id {
                    Some(TrayAction::CheckForUpdates)
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

        Ok(Self {
            tray_icon,
            _menu: menu,
            devices_submenu,
            device_items: Vec::new(),
            autostart_item,
            conn_state: ConnState::Disconnected,
            start_with_windows,
        })
    }

    pub fn update_state(&mut self, state: ConnState) {
        self.conn_state = state;
        self.refresh();
    }

    pub fn set_start_with_windows(&mut self, enabled: bool) {
        self.start_with_windows = enabled;
        self.autostart_item.set_checked(enabled);
    }

    fn refresh(&mut self) {
        self.refresh_device_menu();
        let icon = icon_for_state(self.conn_state.clone());
        let tooltip = match &self.conn_state {
            ConnState::Connected { devices } => {
                let connected = connected_device_count(devices);
                if connected == 1 {
                    "ClipSync \u{b7} 1 device connected".to_string()
                } else {
                    format!("ClipSync \u{b7} {} devices connected", connected)
                }
            }
            ConnState::Connecting { .. } => "ClipSync \u{b7} Connecting\u{2026}".to_string(),
            ConnState::Disconnected => "ClipSync \u{b7} Not connected".to_string(),
        };
        let _ = self.tray_icon.set_icon(Some(icon));
        let _ = self.tray_icon.set_tooltip(Some(&tooltip));
    }

    fn refresh_device_menu(&mut self) {
        for item in self.device_items.drain(..) {
            let _ = self.devices_submenu.remove(&item);
        }

        let devices = devices_for_state(&self.conn_state);
        let connected = connected_device_count(devices);
        self.devices_submenu
            .set_text(format!("Devices: {} connected", connected));

        if devices.is_empty() {
            let item = MenuItem::new("No devices", true, None);
            let _ = self.devices_submenu.append(&item);
            self.device_items.push(item);
            return;
        }

        for device in devices {
            let item = MenuItem::new(device_menu_text(device), true, None);
            let _ = self.devices_submenu.append(&item);
            self.device_items.push(item);
        }
    }
}

fn devices_for_state(state: &ConnState) -> &[DeviceSummary] {
    match state {
        ConnState::Connected { devices } | ConnState::Connecting { devices } => devices,
        ConnState::Disconnected => &[],
    }
}

fn connected_device_count(devices: &[DeviceSummary]) -> usize {
    devices
        .iter()
        .filter(|device| device.state == DeviceSummaryState::Connected)
        .count()
}

fn device_menu_text(device: &DeviceSummary) -> String {
    let label = device
        .name
        .as_ref()
        .map(|name| format!("{}  {}", name, device.ip))
        .unwrap_or_else(|| device.ip.clone());
    format!("{} {}", device_state_symbol(device.state), label)
}

fn device_state_symbol(state: DeviceSummaryState) -> &'static str {
    match state {
        DeviceSummaryState::Connected => "\u{25cf}",
        DeviceSummaryState::Connecting => "\u{25cc}",
        DeviceSummaryState::Disconnected => "\u{25cb}",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn device_menu_uses_symbol_and_short_name() {
        let device = DeviceSummary {
            name: Some("ABC123".to_string()),
            ip: "192.168.0.157".to_string(),
            state: DeviceSummaryState::Connected,
        };

        assert_eq!(device_menu_text(&device), "\u{25cf} ABC123  192.168.0.157");
    }
}
