# Android Clipboard Architecture

This document records the current Android clipboard access path. It is the source of truth for distinguishing the main sync path from legacy Binder diagnostics.

## Main Sync Path

```text
clipsyncd
  -> @clipbridge abstract Unix socket
  -> Zygisk bridge injected into system_server
  -> JNI calls to ServiceManager / IClipboard / ClipboardService
```

`clipsyncd` does not directly call Android's clipboard Binder service. The daemon only speaks the small `@clipbridge` socket protocol:

- `READ\n`
- `WRITE <text>\n`
- `HAS\n`

The Zygisk bridge owns the Android framework interaction. It runs inside `system_server`, obtains the clipboard service through `ServiceManager.getService("clipboard")`, converts it through `IClipboard.Stub.asInterface(...)`, then calls `getPrimaryClip(...)` and `setPrimaryClip(...)`.

## Diagnostic Binder Path

`clipsync-daemon/test_clip.c` is a legacy direct Binder diagnostic. It loads `libbinder_ndk.so` and probes the clipboard service directly. It is useful for debugging platform Binder behavior, but it is not part of the production sync path.

The main `clipsyncd` binary should not link `libbinder_ndk`; only the diagnostic test binary should.

## SELinux Policy

The default module policy no longer grants `ksu` direct access to `clipboard_service`. Main clipboard access happens inside the Zygisk bridge in `system_server`, while `clipsyncd` talks to the bridge over `@clipbridge`.

If a device requires legacy direct Binder permissions during diagnostics, document that as a device-specific compatibility exception instead of treating it as part of the main sync path.

## Future Event Listener Extension

A future Java helper/dex can add event-driven clipboard notifications by registering an `IOnPrimaryClipChangedListener` from the Zygisk bridge. That extension should build on the same bridge boundary:

```text
ClipboardService event -> Zygisk Java helper -> native bridge -> clipsyncd
```

It should not move WebSocket, sync policy, or PC protocol logic into the helper. The helper should only register the listener and notify the bridge when clipboard state changes.
