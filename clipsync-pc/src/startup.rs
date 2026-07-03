use anyhow::Context;
use windows::Win32::System::Registry::{
    RegCloseKey, RegCreateKeyW, RegDeleteValueW, RegOpenKeyExW, RegQueryValueExW,
    RegSetValueExW, HKEY, HKEY_CURRENT_USER, KEY_READ, REG_SZ,
};
use windows::core::w;

pub fn set_autostart(enabled: bool) -> anyhow::Result<()> {
    let exe = std::env::current_exe().context("failed to get current exe path")?;
    let exe_str = exe.to_string_lossy();

    unsafe {
        let mut hkey = HKEY::default();
        RegCreateKeyW(
            HKEY_CURRENT_USER,
            w!("Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
            &mut hkey,
        )
        .ok()
        .context("failed to open Run registry key")?;

        if enabled {
            let data = exe_str.as_bytes();
            RegSetValueExW(
                hkey,
                w!("ClipSync"),
                0,
                REG_SZ,
                Some(data),
            )
            .ok()
            .context("failed to write autostart registry value")?;
            log::info!("Autostart enabled: {}", exe_str);
        } else {
            let _ = RegDeleteValueW(hkey, w!("ClipSync"));
            log::info!("Autostart disabled");
        }

        let _ = RegCloseKey(hkey);
    }
    Ok(())
}

pub fn is_autostart_enabled() -> bool {
    unsafe {
        let mut hkey = HKEY::default();
        if RegOpenKeyExW(HKEY_CURRENT_USER, w!("Software\\Microsoft\\Windows\\CurrentVersion\\Run"), 0, KEY_READ, &mut hkey).is_err() {
            return false;
        }
        let mut ty = windows::Win32::System::Registry::REG_VALUE_TYPE(0);
        let mut len = 0u32;
        let result = RegQueryValueExW(hkey, w!("ClipSync"), None, Some(&mut ty), None, Some(&mut len));
        let _ = RegCloseKey(hkey);
        result.is_ok() && len > 0
    }
}
