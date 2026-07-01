use std::sync::atomic::{AtomicBool, Ordering};
use tokio::sync::mpsc;
use windows::core::w;
use windows::Win32::Foundation::*;
use windows::Win32::System::DataExchange::*;
use windows::Win32::System::LibraryLoader::*;
use windows::Win32::System::Memory::*;
use windows::Win32::System::Ole::*;
use windows::Win32::UI::WindowsAndMessaging::*;

static SELF_WRITING: AtomicBool = AtomicBool::new(false);

pub fn set_self_writing(v: bool) {
    SELF_WRITING.store(v, Ordering::SeqCst);
}

pub fn is_self_writing() -> bool {
    SELF_WRITING.load(Ordering::SeqCst)
}

pub fn read() -> Option<String> {
    unsafe {
        if OpenClipboard(None).is_err() {
            return None;
        }
        let result = read_inner();
        let _ = CloseClipboard();
        result
    }
}

unsafe fn read_inner() -> Option<String> {
    let handle = GetClipboardData(CF_UNICODETEXT.0 as u32).ok()?;
    if handle.is_invalid() {
        return None;
    }
    let ptr = GlobalLock(HGLOBAL(handle.0)) as *const u16;
    if ptr.is_null() {
        return None;
    }
    let len = (0..).take_while(|&i| *ptr.add(i) != 0).count();
    let slice = std::slice::from_raw_parts(ptr, len);
    let text = String::from_utf16_lossy(slice);
    let _ = GlobalUnlock(HGLOBAL(handle.0));
    Some(text)
}

pub fn write(text: &str) -> bool {
    unsafe {
        set_self_writing(true);
        if OpenClipboard(None).is_err() {
            set_self_writing(false);
            return false;
        }
        let _ = EmptyClipboard();

        let wide: Vec<u16> = text
            .encode_utf16()
            .chain(std::iter::once(0))
            .collect();
        let byte_size = wide.len() * std::mem::size_of::<u16>();
        let hmem = match GlobalAlloc(GMEM_MOVEABLE, byte_size) {
            Ok(h) => h,
            Err(_) => {
                let _ = CloseClipboard();
                set_self_writing(false);
                return false;
            }
        };
        if hmem.is_invalid() {
            let _ = CloseClipboard();
            set_self_writing(false);
            return false;
        }
        let dst = GlobalLock(hmem) as *mut u16;
        if !dst.is_null() {
            std::ptr::copy_nonoverlapping(wide.as_ptr(), dst, wide.len());
            let _ = GlobalUnlock(hmem);
        }
        let result = SetClipboardData(CF_UNICODETEXT.0 as u32, HANDLE(hmem.0));
        let _ = CloseClipboard();
        let ok = result.is_ok_and(|h| !h.is_invalid());
        set_self_writing(false);
        ok
    }
}

pub struct ClipListener {
    tx: mpsc::UnboundedSender<String>,
}

impl ClipListener {
    pub fn new(tx: mpsc::UnboundedSender<String>) -> Self {
        Self { tx }
    }

    pub fn spawn(self) {
        std::thread::spawn(move || unsafe { message_loop(self.tx) });
    }
}

static mut TX_PTR: Option<mpsc::UnboundedSender<String>> = None;

unsafe fn message_loop(tx: mpsc::UnboundedSender<String>) {
    TX_PTR = Some(tx);

    let hmodule: HMODULE = match GetModuleHandleW(None) {
        Ok(h) => h,
        Err(_) => {
            log::error!("GetModuleHandleW failed");
            return;
        }
    };
    let class_name = w!("ClipSyncClipListener");

    let wc = WNDCLASSW {
        lpfnWndProc: Some(wndproc),
        hInstance: HINSTANCE(hmodule.0),
        lpszClassName: class_name,
        ..Default::default()
    };

    if RegisterClassW(&wc) == 0 {
        log::error!("RegisterClassW failed");
        return;
    }

    let hwnd = match CreateWindowExW(
        WINDOW_EX_STYLE::default(),
        class_name,
        w!("ClipSync"),
        WINDOW_STYLE::default(),
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        None,
        None,
        hmodule,
        None,
    ) {
        Ok(h) => h,
        Err(_) => {
            log::error!("CreateWindowExW failed");
            return;
        }
    };

    if AddClipboardFormatListener(hwnd).is_err() {
        log::error!("AddClipboardFormatListener failed");
        return;
    }

    let mut msg = MSG::default();
    loop {
        let ret = GetMessageW(&mut msg, None, 0, 0);
        if ret.0 <= 0 {
            break;
        }
        let _ = TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    let _ = RemoveClipboardFormatListener(hwnd);
    let _ = DestroyWindow(hwnd);
}

unsafe extern "system" fn wndproc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    match msg {
        WM_CLIPBOARDUPDATE => {
            if !SELF_WRITING.load(Ordering::SeqCst) {
                if let Some(ref tx) = TX_PTR {
                    if let Some(text) = read() {
                        let _ = tx.send(text);
                    }
                }
            }
            LRESULT(0)
        }
        WM_DESTROY => {
            PostQuitMessage(0);
            LRESULT(0)
        }
        _ => DefWindowProcW(hwnd, msg, wparam, lparam),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_read_write_roundtrip() {
        let test_text = "ClipSync-test-hello";
        assert!(write(test_text));
        let read_back = read().unwrap();
        assert_eq!(read_back, test_text);
    }

    #[test]
    fn test_write_clears_self_writing_flag() {
        write("test");
        assert!(!is_self_writing());
    }
}
