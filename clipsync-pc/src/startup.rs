use anyhow::Context;
use windows::core::w;
use windows::Win32::System::Registry::{
    RegCloseKey, RegCreateKeyW, RegDeleteValueW, RegSetValueExW, HKEY, HKEY_CURRENT_USER, REG_SZ,
};

pub fn set_autostart(enabled: bool) -> anyhow::Result<()> {
    let exe = std::env::current_exe().context("failed to get current exe path")?;

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
            use std::os::windows::ffi::OsStrExt;
            let wide: Vec<u16> = exe
                .as_os_str()
                .encode_wide()
                .chain(std::iter::once(0))
                .collect();
            let data = std::slice::from_raw_parts(
                wide.as_ptr() as *const u8,
                wide.len() * std::mem::size_of::<u16>(),
            );
            RegSetValueExW(hkey, w!("ClipSync"), 0, REG_SZ, Some(data))
                .ok()
                .context("failed to write autostart registry value")?;
            log::info!("Autostart enabled: {}", exe.display());
        } else {
            let _ = RegDeleteValueW(hkey, w!("ClipSync"));
            log::info!("Autostart disabled");
        }

        let _ = RegCloseKey(hkey);
    }
    Ok(())
}
