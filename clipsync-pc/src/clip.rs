use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
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

#[cfg(test)]
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

        let wide: Vec<u16> = text.encode_utf16().chain(std::iter::once(0)).collect();
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

pub struct ClipDeduper {
    last: Option<String>,
}

impl ClipDeduper {
    #[cfg(test)]
    pub fn new() -> Self {
        Self { last: None }
    }

    pub fn with_initial(initial: Option<String>) -> Self {
        Self { last: initial }
    }

    pub fn should_emit(&mut self, text: &str) -> bool {
        if self.last.as_deref() == Some(text) {
            return false;
        }
        self.last = Some(text.to_string());
        true
    }
}

pub struct ClipListener {
    tx: mpsc::UnboundedSender<String>,
}

struct ListenerContext {
    tx: mpsc::UnboundedSender<String>,
    deduper: Arc<Mutex<ClipDeduper>>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PollingFallback {
    Start,
    Skip,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct ListenerStartupPlan {
    start_message_listener: bool,
    start_polling_fallback: bool,
}

fn polling_fallback_for_message_listener(message_listener_started: bool) -> PollingFallback {
    if message_listener_started {
        PollingFallback::Skip
    } else {
        PollingFallback::Start
    }
}

fn listener_startup_plan(message_listener_available: bool) -> ListenerStartupPlan {
    let fallback = polling_fallback_for_message_listener(message_listener_available);
    ListenerStartupPlan {
        start_message_listener: message_listener_available,
        start_polling_fallback: fallback == PollingFallback::Start,
    }
}

impl ClipListener {
    pub fn new(tx: mpsc::UnboundedSender<String>) -> Self {
        Self { tx }
    }

    pub fn spawn(self) {
        let deduper = Arc::new(Mutex::new(ClipDeduper::with_initial(read())));
        let message_listener_started =
            spawn_message_listener(self.tx.clone(), Arc::clone(&deduper));
        let plan = listener_startup_plan(message_listener_started);

        if plan.start_message_listener && !plan.start_polling_fallback {
            log::info!("Windows clipboard polling fallback skipped");
        }
        if plan.start_polling_fallback {
            log::warn!("Windows clipboard message listener unavailable; starting polling fallback");
            std::thread::spawn(move || poll_loop(self.tx, deduper));
        }
    }
}

fn spawn_message_listener(
    tx: mpsc::UnboundedSender<String>,
    deduper: Arc<Mutex<ClipDeduper>>,
) -> bool {
    let (ready_tx, ready_rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || unsafe { message_loop(tx, deduper, ready_tx) });
    ready_rx.recv().unwrap_or(false)
}

unsafe fn message_loop(
    tx: mpsc::UnboundedSender<String>,
    deduper: Arc<Mutex<ClipDeduper>>,
    ready_tx: std::sync::mpsc::Sender<bool>,
) {
    log::info!("Windows clipboard message listener starting");

    let hmodule: HMODULE = match GetModuleHandleW(None) {
        Ok(h) => h,
        Err(_) => {
            log::error!("GetModuleHandleW failed");
            let _ = ready_tx.send(false);
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
        let _ = ready_tx.send(false);
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
            let _ = ready_tx.send(false);
            return;
        }
    };

    if AddClipboardFormatListener(hwnd).is_err() {
        log::error!("AddClipboardFormatListener failed");
        let _ = DestroyWindow(hwnd);
        let _ = ready_tx.send(false);
        return;
    }

    let context = Box::new(ListenerContext { tx, deduper });
    let _ = SetWindowLongPtrW(hwnd, GWLP_USERDATA, Box::into_raw(context) as isize);

    log::info!("Windows clipboard message listener ready");
    let _ = ready_tx.send(true);

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

fn poll_loop(tx: mpsc::UnboundedSender<String>, deduper: Arc<Mutex<ClipDeduper>>) {
    log::info!("Windows clipboard polling fallback started");
    loop {
        std::thread::sleep(Duration::from_millis(500));
        if SELF_WRITING.load(Ordering::SeqCst) {
            continue;
        }
        if let Some(text) = read() {
            emit_if_changed(&tx, &deduper, text, "poll");
        }
    }
}

fn emit_if_changed(
    tx: &mpsc::UnboundedSender<String>,
    deduper: &Arc<Mutex<ClipDeduper>>,
    text: String,
    source: &str,
) {
    let Ok(mut guard) = deduper.lock() else {
        log::error!("clipboard deduper lock poisoned");
        return;
    };
    if guard.should_emit(&text) {
        log::info!(
            "PC clipboard changed via {}: {} chars",
            source,
            text.chars().count()
        );
        let _ = tx.send(text);
    }
}

unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    match msg {
        WM_CLIPBOARDUPDATE => {
            log::debug!("WM_CLIPBOARDUPDATE received");
            if !SELF_WRITING.load(Ordering::SeqCst) {
                if let Some(text) = read() {
                    let ptr = GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *const ListenerContext;
                    if !ptr.is_null() {
                        let context = &*ptr;
                        emit_if_changed(&context.tx, &context.deduper, text, "message");
                    }
                }
            }
            LRESULT(0)
        }
        WM_DESTROY => {
            let ptr = GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *mut ListenerContext;
            if !ptr.is_null() {
                let _ = Box::from_raw(ptr);
                let _ = SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            PostQuitMessage(0);
            LRESULT(0)
        }
        _ => DefWindowProcW(hwnd, msg, wparam, lparam),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    static CLIPBOARD_TEST_LOCK: Mutex<()> = Mutex::new(());

    #[test]
    #[ignore = "requires an interactive Windows clipboard"]
    fn test_read_write_roundtrip() {
        let _guard = CLIPBOARD_TEST_LOCK.lock().unwrap();
        let test_text = "ClipSync-test-hello";
        assert!(write(test_text));
        let read_back = read().unwrap();
        assert_eq!(read_back, test_text);
    }

    #[test]
    fn test_write_clears_self_writing_flag() {
        let _guard = CLIPBOARD_TEST_LOCK.lock().unwrap();
        write("test");
        assert!(!is_self_writing());
    }

    #[test]
    fn test_clip_deduper_emits_only_changed_text() {
        let mut deduper = ClipDeduper::new();
        assert!(deduper.should_emit("hello"));
        assert!(!deduper.should_emit("hello"));
        assert!(deduper.should_emit("world"));
    }

    #[test]
    fn polling_fallback_is_skipped_when_message_listener_starts() {
        assert_eq!(
            polling_fallback_for_message_listener(true),
            PollingFallback::Skip
        );
    }

    #[test]
    fn listener_startup_skips_polling_when_message_listener_is_available() {
        assert_eq!(
            listener_startup_plan(true),
            ListenerStartupPlan {
                start_message_listener: true,
                start_polling_fallback: false
            }
        );
    }
}
