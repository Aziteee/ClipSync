# Android WebSocket Server And mDNS Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Android daemon's WebSocket and mDNS placeholders with a real LAN WebSocket server, HMAC authentication, clipboard message routing, and `_clipsync._tcp.local.` discovery.

**Architecture:** `clipsyncd` remains a single-process Android daemon. Mongoose owns the network event manager, `ws_server` handles HTTP upgrade/WebSocket/auth/client state, `mdns_publish` registers DNS-SD records on the same event manager, and Binder clipboard code stays behind the existing callback boundary. Protocol helpers isolate JSON building/parsing and HMAC verification so WebSocket code is lifecycle logic rather than string manipulation.

**Tech Stack:** C17-compatible C via Android NDK, Mongoose 7.21 vendored as `mongoose.c/h`, Android BSD sockets, Mongoose WebSocket API, Mongoose mDNS/DNS-SD API, local SHA-256/HMAC-SHA256 helper, PowerShell adb smoke tests.

---

## Current State

- `clipsync-daemon/ws_server.c` is a placeholder: `ws_server_init()` prints, `ws_server_poll()` sleeps, and `ws_server_broadcast()` prints.
- `clipsync-daemon/mdns_publish.c` is a placeholder and does not register `_clipsync._tcp.local.`
- `clipsync-daemon/clipsyncd.c` already has the right integration shape: Binder callback calls `ws_server_broadcast()`, incoming WebSocket set should call `binder_clip_set_text()`.
- `clipsync-pc/src/ws.rs` already expects this handshake:
  - server sends `{"type":"hello","challenge":"<hex>"}`
  - client sends `{"type":"auth","response":"<hmac-sha256-hex>"}`
  - server sends `{"type":"auth_ok"}` or `{"type":"auth_fail"}`
- `clipsync-pc/src/mdns.rs` browses `_clipsync._tcp.local.` and constructs `ws://<addr>:<port>`.

## External References

- Mongoose WebSocket API: `mg_ws_upgrade()`, `mg_ws_send()`, `MG_EV_WS_OPEN`, `MG_EV_WS_MSG`.
  Source: `https://mongoose.ws/docs/api/websocket/`
- Mongoose mDNS/DNS-SD API: `mg_mdns_listen()`, `MG_EV_MDNS_REQ`, `struct mg_dnssd_record`.
  Source: `https://mongoose.ws/docs/api/dns/`
- Mongoose 7.21 release adds DNS-SD support and the new mDNS API.
  Source: `https://github.com/cesanta/mongoose/releases/tag/7.21`

## File Structure

- Create: `clipsync-daemon/third_party/mongoose/mongoose.c`
- Create: `clipsync-daemon/third_party/mongoose/mongoose.h`
- Create: `clipsync-daemon/third_party/mongoose/MONGOOSE_VERSION.txt`
- Create: `clipsync-daemon/crypto_hmac.h`
- Create: `clipsync-daemon/crypto_hmac.c`
- Create: `clipsync-daemon/protocol_json.h`
- Create: `clipsync-daemon/protocol_json.c`
- Create: `clipsync-daemon/test_crypto_hmac.c`
- Create: `clipsync-daemon/test_protocol_json.c`
- Create: `clipsync-daemon/tools/ws_smoke.ps1`
- Modify: `clipsync-daemon/Makefile`
- Modify: `clipsync-daemon/protocol.h`
- Modify: `clipsync-daemon/ws_server.h`
- Modify: `clipsync-daemon/ws_server.c`
- Modify: `clipsync-daemon/mdns_publish.h`
- Modify: `clipsync-daemon/mdns_publish.c`
- Modify: `clipsync-daemon/clipsyncd.c`
- Modify: `clipsync-daemon/test_daemon_contract.ps1`

## Behavioral Contract

- Listen on `0.0.0.0:5287` by default.
- Accept WebSocket upgrade at `/` and `/ws`.
- Reject plain HTTP with `404`.
- Send `hello` only after WebSocket open.
- Generate a fresh 32-byte challenge per connection, hex encoded to 64 lowercase characters.
- Before auth, accept only `auth`; ignore `ping`, reject `clipboard_set`, and close malformed clients after sending `auth_fail`.
- Use constant-time comparison for HMAC hex responses.
- Broadcast clipboard pushes only to authenticated clients.
- Handle incoming authenticated `clipboard_set` by calling `ws_on_set_fn`.
- Reply to protocol-level `ping` with protocol-level `pong`.
- Publish DNS-SD service `_clipsync._tcp.local.` with instance `ClipSync Android`, port `5287`, TXT records `version=1`, `proto=json`, `auth=hmac-sha256`.
- Keep Mongoose polling non-busy: `ws_server_poll(50)` maps to `mg_mgr_poll(&mgr, 50)`.

---

### Task 1: Vendor Mongoose 7.21 And Wire The Build

**Files:**
- Create: `clipsync-daemon/third_party/mongoose/mongoose.c`
- Create: `clipsync-daemon/third_party/mongoose/mongoose.h`
- Create: `clipsync-daemon/third_party/mongoose/MONGOOSE_VERSION.txt`
- Modify: `clipsync-daemon/Makefile`

- [ ] **Step 1: Download the pinned Mongoose files**

Run from `D:\Projects\rust_proj\ClipSync\clipsync-daemon`:

```powershell
New-Item -ItemType Directory -Force third_party\mongoose | Out-Null
Invoke-WebRequest -Uri "https://raw.githubusercontent.com/cesanta/mongoose/7.21/mongoose.c" -OutFile "third_party\mongoose\mongoose.c"
Invoke-WebRequest -Uri "https://raw.githubusercontent.com/cesanta/mongoose/7.21/mongoose.h" -OutFile "third_party\mongoose\mongoose.h"
@"
Mongoose version: 7.21
Source: https://github.com/cesanta/mongoose/releases/tag/7.21
Vendored for: Android daemon WebSocket server and DNS-SD/mDNS responder
License note: Mongoose is GPLv2 or commercial. Confirm this repository's distribution model before publishing binaries.
"@ | Set-Content -Encoding ASCII third_party\mongoose\MONGOOSE_VERSION.txt
```

Expected: `third_party/mongoose/mongoose.c` and `mongoose.h` exist.

- [ ] **Step 2: Add Mongoose to the daemon build**

Modify `clipsync-daemon/Makefile`:

```makefile
BUILD_DIR = build
CC ?= aarch64-linux-android33-clang
CFLAGS = -Wall -Wextra -O2 -Ithird_party/mongoose -DMG_ENABLE_MDNS=1
LDFLAGS = -lbinder_ndk -ldl
TARGET = $(BUILD_DIR)/clipsyncd
SRCS = clipsyncd.c binder_clip.c ws_server.c mdns_publish.c protocol_json.c crypto_hmac.c third_party/mongoose/mongoose.c
OBJS = $(SRCS:%.c=$(BUILD_DIR)/%.o)
NDK_BUILD ?= ndk-build
ZYGISK_SO = module/zygisk/arm64-v8a.so

all: $(TARGET) $(BUILD_DIR)/test_clip $(BUILD_DIR)/test_crypto_hmac $(BUILD_DIR)/test_protocol_json $(ZYGISK_SO)

$(BUILD_DIR):
	@if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)
	@if not exist $(BUILD_DIR)\third_party\mongoose mkdir $(BUILD_DIR)\third_party\mongoose

$(TARGET): $(OBJS) protocol.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/third_party/mongoose/mongoose.o: third_party/mongoose/mongoose.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<
```

Keep the existing `$(ZYGISK_SO)`, `clean`, `install`, `module`, and `test_clip` rules, then add the test binary rules from Tasks 2 and 3.

- [ ] **Step 3: Verify compile reaches expected missing-symbol errors**

Run:

```powershell
$env:ANDROID_NDK_ROOT = "D:\AppData\Android\sdk\ndk\30.0.14904198"
$env:PATH = "$env:ANDROID_NDK_ROOT;$env:ANDROID_NDK_ROOT\toolchains\llvm\prebuilt\windows-x86_64\bin;$env:ANDROID_NDK_ROOT\prebuilt\windows-x86_64\bin;$env:PATH"
$env:CC = "aarch64-linux-android33-clang"
make build\third_party\mongoose\mongoose.o
```

Expected: `build/third_party/mongoose/mongoose.o` is created.

- [ ] **Step 4: Commit**

```powershell
git add clipsync-daemon/third_party/mongoose clipsync-daemon/Makefile
git commit -m "build: vendor mongoose for Android daemon networking"
```

---

### Task 2: Add HMAC-SHA256 Helper With Device Tests

**Files:**
- Create: `clipsync-daemon/crypto_hmac.h`
- Create: `clipsync-daemon/crypto_hmac.c`
- Create: `clipsync-daemon/test_crypto_hmac.c`
- Modify: `clipsync-daemon/Makefile`

- [ ] **Step 1: Define the crypto interface**

Create `clipsync-daemon/crypto_hmac.h`:

```c
#ifndef CLIPSYNC_CRYPTO_HMAC_H
#define CLIPSYNC_CRYPTO_HMAC_H

#include <stddef.h>

#define SHA256_DIGEST_LEN 32
#define SHA256_HEX_LEN 64

int clipsync_hmac_sha256_hex(const char *key, const char *message, char out_hex[SHA256_HEX_LEN + 1]);
int clipsync_secure_hex_eq(const char *a, const char *b, size_t n);

#endif
```

- [ ] **Step 2: Implement SHA-256, HMAC, and constant-time compare**

Create `clipsync-daemon/crypto_hmac.c` using a compact SHA-256 implementation local to this file and expose only the two functions from the header. The implementation must satisfy:

```c
int clipsync_hmac_sha256_hex(const char *key, const char *message, char out_hex[65]) {
    /* Rules:
       1. Treat NULL key/message as empty strings.
       2. If key length > 64, hash it first.
       3. ipad byte = 0x36, opad byte = 0x5c.
       4. Output lowercase hex and terminate with '\0'.
       5. Return 0 on success, -1 on invalid out_hex.
    */
}

int clipsync_secure_hex_eq(const char *a, const char *b, size_t n) {
    /* Rules:
       1. Return 0 if either pointer is NULL.
       2. XOR every byte before deciding.
       3. Return 1 on equality, 0 otherwise.
    */
}
```

- [ ] **Step 3: Add known-answer tests**

Create `clipsync-daemon/test_crypto_hmac.c`:

```c
#include <stdio.h>
#include <string.h>
#include "crypto_hmac.h"

static int expect_eq(const char *name, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s failed\n got:  %s\n want: %s\n", name, got, want);
        return 1;
    }
    return 0;
}

int main(void) {
    char out[SHA256_HEX_LEN + 1];
    int failures = 0;

    failures += clipsync_hmac_sha256_hex(
        "key",
        "The quick brown fox jumps over the lazy dog",
        out
    );
    failures += expect_eq(
        "RFC4231-like quick brown fox",
        out,
        "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"
    );

    failures += clipsync_hmac_sha256_hex("", "", out);
    failures += expect_eq(
        "empty key empty message",
        out,
        "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad"
    );

    if (!clipsync_secure_hex_eq("abcdef", "abcdef", 6)) {
        fprintf(stderr, "secure compare equality failed\n");
        failures++;
    }
    if (clipsync_secure_hex_eq("abcdef", "abcdeg", 6)) {
        fprintf(stderr, "secure compare inequality failed\n");
        failures++;
    }

    if (failures == 0) {
        printf("crypto_hmac tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 4: Add Makefile test target**

Add:

```makefile
$(BUILD_DIR)/test_crypto_hmac: test_crypto_hmac.c crypto_hmac.c crypto_hmac.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_crypto_hmac.c crypto_hmac.c

test-crypto: $(BUILD_DIR)/test_crypto_hmac
	adb push $(BUILD_DIR)/test_crypto_hmac /data/local/tmp/
	adb shell su -c "chmod 755 /data/local/tmp/test_crypto_hmac && /data/local/tmp/test_crypto_hmac"
```

Add `test-crypto` to `.PHONY`.

- [ ] **Step 5: Run on device**

```powershell
make test-crypto
```

Expected: `crypto_hmac tests passed`.

- [ ] **Step 6: Commit**

```powershell
git add clipsync-daemon/crypto_hmac.* clipsync-daemon/test_crypto_hmac.c clipsync-daemon/Makefile
git commit -m "feat: add daemon HMAC-SHA256 helper"
```

---

### Task 3: Add Protocol JSON Helpers

**Files:**
- Create: `clipsync-daemon/protocol_json.h`
- Create: `clipsync-daemon/protocol_json.c`
- Create: `clipsync-daemon/test_protocol_json.c`
- Modify: `clipsync-daemon/Makefile`

- [ ] **Step 1: Define ownership-safe protocol helpers**

Create `clipsync-daemon/protocol_json.h`:

```c
#ifndef CLIPSYNC_PROTOCOL_JSON_H
#define CLIPSYNC_PROTOCOL_JSON_H

#include <stddef.h>

typedef enum {
    CLIPSYNC_MSG_UNKNOWN = 0,
    CLIPSYNC_MSG_CLIPBOARD_SET,
    CLIPSYNC_MSG_PING,
    CLIPSYNC_MSG_PONG,
    CLIPSYNC_MSG_AUTH
} clipsync_msg_type;

clipsync_msg_type protocol_json_type(const char *json, size_t len);
char *protocol_json_get_text(const char *json, size_t len);
char *protocol_json_get_auth_response(const char *json, size_t len);
char *protocol_json_build_hello(const char *challenge_hex);
char *protocol_json_build_auth_ok(void);
char *protocol_json_build_auth_fail(void);
char *protocol_json_build_pong(void);
char *protocol_json_build_clipboard_push(const char *text, unsigned long long ts);
char *protocol_json_escape(const char *text);

#endif
```

- [ ] **Step 2: Implement helpers with Mongoose JSON APIs**

Create `clipsync-daemon/protocol_json.c` with these rules:

```c
/*
Implementation rules:
1. Use mg_json_get_str(mg_str_n(json, len), "$.type") for type/auth/text extraction.
2. Caller owns every returned char* and frees it with free().
3. protocol_json_escape() escapes ", \, \b, \f, \n, \r, \t and preserves UTF-8 bytes.
4. protocol_json_build_clipboard_push() always escapes text before formatting JSON.
5. Unknown or malformed JSON returns CLIPSYNC_MSG_UNKNOWN and NULL fields.
*/
```

The type mapping must be exact:

```c
if (strcmp(type, "clipboard_set") == 0) return CLIPSYNC_MSG_CLIPBOARD_SET;
if (strcmp(type, "ping") == 0) return CLIPSYNC_MSG_PING;
if (strcmp(type, "pong") == 0) return CLIPSYNC_MSG_PONG;
if (strcmp(type, "auth") == 0) return CLIPSYNC_MSG_AUTH;
```

- [ ] **Step 3: Add JSON tests**

Create `clipsync-daemon/test_protocol_json.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol_json.h"

static int fail(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    return 1;
}

int main(void) {
    int failures = 0;

    const char *set = "{\"type\":\"clipboard_set\",\"text\":\"hello\"}";
    if (protocol_json_type(set, strlen(set)) != CLIPSYNC_MSG_CLIPBOARD_SET) {
        failures += fail("clipboard_set type parse failed");
    }

    char *text = protocol_json_get_text(set, strlen(set));
    if (!text || strcmp(text, "hello") != 0) {
        failures += fail("text parse failed");
    }
    free(text);

    const char *auth = "{\"type\":\"auth\",\"response\":\"abc123\"}";
    char *response = protocol_json_get_auth_response(auth, strlen(auth));
    if (!response || strcmp(response, "abc123") != 0) {
        failures += fail("auth response parse failed");
    }
    free(response);

    char *escaped = protocol_json_escape("quote:\" slash:\\ line:\n");
    if (!escaped || strcmp(escaped, "quote:\\\" slash:\\\\ line:\\n") != 0) {
        failures += fail("json escape failed");
    }
    free(escaped);

    char *push = protocol_json_build_clipboard_push("hi \"pc\"", 1234);
    if (!push || strcmp(push, "{\"type\":\"clipboard_push\",\"text\":\"hi \\\"pc\\\"\",\"ts\":1234}") != 0) {
        failures += fail("clipboard_push build failed");
    }
    free(push);

    char *hello = protocol_json_build_hello("001122");
    if (!hello || strcmp(hello, "{\"type\":\"hello\",\"challenge\":\"001122\"}") != 0) {
        failures += fail("hello build failed");
    }
    free(hello);

    if (failures == 0) {
        printf("protocol_json tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 4: Add Makefile test target**

Add:

```makefile
$(BUILD_DIR)/test_protocol_json: test_protocol_json.c protocol_json.c protocol_json.h third_party/mongoose/mongoose.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_protocol_json.c protocol_json.c third_party/mongoose/mongoose.c

test-json: $(BUILD_DIR)/test_protocol_json
	adb push $(BUILD_DIR)/test_protocol_json /data/local/tmp/
	adb shell su -c "chmod 755 /data/local/tmp/test_protocol_json && /data/local/tmp/test_protocol_json"
```

Add `test-json` to `.PHONY`.

- [ ] **Step 5: Run on device**

```powershell
make test-json
```

Expected: `protocol_json tests passed`.

- [ ] **Step 6: Commit**

```powershell
git add clipsync-daemon/protocol_json.* clipsync-daemon/test_protocol_json.c clipsync-daemon/Makefile
git commit -m "feat: add daemon protocol JSON helpers"
```

---

### Task 4: Replace WebSocket Placeholder With Real Mongoose Server

**Files:**
- Modify: `clipsync-daemon/ws_server.h`
- Modify: `clipsync-daemon/ws_server.c`
- Modify: `clipsync-daemon/test_daemon_contract.ps1`

- [ ] **Step 1: Expand `ws_server.h`**

Replace `clipsync-daemon/ws_server.h` with:

```c
#ifndef WS_SERVER_H
#define WS_SERVER_H

#include <stddef.h>

struct mg_mgr;

typedef void (*ws_on_set_fn)(const char *text);

int  ws_server_init(int port, const char *secret);
void ws_server_poll(int timeout_ms);
void ws_server_cleanup(void);
void ws_server_broadcast(const char *json);
void ws_server_set_on_set(ws_on_set_fn fn);
struct mg_mgr *ws_server_mgr(void);
size_t ws_server_authenticated_count(void);

#endif
```

- [ ] **Step 2: Implement server state**

In `clipsync-daemon/ws_server.c`, define:

```c
#include "ws_server.h"
#include "protocol.h"
#include "protocol_json.h"
#include "crypto_hmac.h"
#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CLIENTS 4
#define CHALLENGE_BYTES 32
#define CHALLENGE_HEX_LEN 64

typedef struct {
    struct mg_connection *conn;
    int authenticated;
    char challenge[CHALLENGE_HEX_LEN + 1];
} ws_client;

static struct mg_mgr g_mgr;
static struct mg_connection *g_listener = NULL;
static ws_client g_clients[MAX_CLIENTS];
static char g_secret[256];
static ws_on_set_fn g_on_set = NULL;
static int g_initialized = 0;
```

Add helpers:

```c
static ws_client *client_find(struct mg_connection *c);
static ws_client *client_alloc(struct mg_connection *c);
static void client_free(struct mg_connection *c);
static int make_challenge(char out[CHALLENGE_HEX_LEN + 1]);
static void send_text(struct mg_connection *c, const char *json);
static void handle_auth(struct mg_connection *c, ws_client *client, const char *json, size_t len);
static void handle_authed_message(struct mg_connection *c, const char *json, size_t len);
static void ws_event_handler(struct mg_connection *c, int ev, void *ev_data);
```

- [ ] **Step 3: Implement lifecycle functions**

`ws_server_init()` must:

```c
int ws_server_init(int port, const char *secret) {
    char listen_url[64];
    if (g_initialized) return 0;

    memset(g_clients, 0, sizeof(g_clients));
    memset(g_secret, 0, sizeof(g_secret));
    if (secret) {
        strncpy(g_secret, secret, sizeof(g_secret) - 1);
    }

    mg_mgr_init(&g_mgr);
    snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%d", port);
    g_listener = mg_http_listen(&g_mgr, listen_url, ws_event_handler, NULL);
    if (!g_listener) {
        fprintf(stderr, "[ws_server] failed to listen on %s\n", listen_url);
        mg_mgr_free(&g_mgr);
        return -1;
    }

    g_initialized = 1;
    printf("[ws_server] listening on %s\n", listen_url);
    return 0;
}
```

`ws_server_poll()` must call `mg_mgr_poll(&g_mgr, timeout_ms)` when initialized.

`ws_server_cleanup()` must call `mg_mgr_free(&g_mgr)` once and reset all globals.

`ws_server_mgr()` must return `g_initialized ? &g_mgr : NULL`.

- [ ] **Step 4: Implement WebSocket events**

The event handler must follow this exact flow:

```c
static void ws_event_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        if (mg_match(hm->uri, mg_str("/"), NULL) || mg_match(hm->uri, mg_str("/ws"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
        } else {
            mg_http_reply(c, 404, "", "not found\n");
        }
        return;
    }

    if (ev == MG_EV_WS_OPEN) {
        ws_client *client = client_alloc(c);
        if (!client || make_challenge(client->challenge) != 0) {
            c->is_closing = 1;
            return;
        }
        char *hello = protocol_json_build_hello(client->challenge);
        send_text(c, hello);
        free(hello);
        return;
    }

    if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
        ws_client *client = client_find(c);
        if (!client) {
            c->is_closing = 1;
            return;
        }
        if (!client->authenticated) {
            handle_auth(c, client, wm->data.buf, wm->data.len);
        } else {
            handle_authed_message(c, wm->data.buf, wm->data.len);
        }
        return;
    }

    if (ev == MG_EV_CLOSE) {
        client_free(c);
    }
}
```

- [ ] **Step 5: Implement authentication**

`handle_auth()` must:

```c
static void handle_auth(struct mg_connection *c, ws_client *client, const char *json, size_t len) {
    if (protocol_json_type(json, len) != CLIPSYNC_MSG_AUTH) {
        char *fail = protocol_json_build_auth_fail();
        send_text(c, fail);
        free(fail);
        c->is_closing = 1;
        return;
    }

    char expected[SHA256_HEX_LEN + 1];
    char *response = protocol_json_get_auth_response(json, len);
    if (!response || clipsync_hmac_sha256_hex(g_secret, client->challenge, expected) != 0 ||
        strlen(response) != SHA256_HEX_LEN ||
        !clipsync_secure_hex_eq(response, expected, SHA256_HEX_LEN)) {
        free(response);
        char *fail = protocol_json_build_auth_fail();
        send_text(c, fail);
        free(fail);
        c->is_closing = 1;
        return;
    }

    free(response);
    client->authenticated = 1;
    char *ok = protocol_json_build_auth_ok();
    send_text(c, ok);
    free(ok);
}
```

- [ ] **Step 6: Implement authenticated message routing**

`handle_authed_message()` must:

```c
static void handle_authed_message(struct mg_connection *c, const char *json, size_t len) {
    clipsync_msg_type type = protocol_json_type(json, len);
    if (type == CLIPSYNC_MSG_PING) {
        char *pong = protocol_json_build_pong();
        send_text(c, pong);
        free(pong);
        return;
    }
    if (type == CLIPSYNC_MSG_CLIPBOARD_SET) {
        char *text = protocol_json_get_text(json, len);
        if (text && g_on_set) {
            g_on_set(text);
        }
        free(text);
        return;
    }
    (void)c;
}
```

- [ ] **Step 7: Implement broadcast**

`ws_server_broadcast()` must:

```c
void ws_server_broadcast(const char *json) {
    if (!g_initialized || !json) return;
    size_t len = strlen(json);
    for (size_t i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].conn && g_clients[i].authenticated) {
            mg_ws_send(g_clients[i].conn, json, len, WEBSOCKET_OP_TEXT);
        }
    }
}
```

- [ ] **Step 8: Update contract test**

Modify `clipsync-daemon/test_daemon_contract.ps1` to assert:

```powershell
if ($wsServer -notmatch "mg_http_listen") {
    throw "ws_server_init must create a real Mongoose HTTP listener"
}

if ($wsServer -notmatch "mg_mgr_poll") {
    throw "ws_server_poll must drive the Mongoose event manager"
}

if ($wsServer -notmatch "mg_ws_upgrade") {
    throw "WebSocket server must upgrade HTTP connections"
}

if ($wsServer -notmatch "mg_ws_send") {
    throw "WebSocket server must send real WebSocket frames"
}
```

Remove the old assertion that requires `usleep`.

- [ ] **Step 9: Build**

```powershell
make build\clipsyncd
.\test_daemon_contract.ps1
```

Expected: daemon compiles and contract checks pass.

- [ ] **Step 10: Commit**

```powershell
git add clipsync-daemon/ws_server.* clipsync-daemon/test_daemon_contract.ps1
git commit -m "feat: implement daemon WebSocket server"
```

---

### Task 5: Implement mDNS/DNS-SD Publisher

**Files:**
- Modify: `clipsync-daemon/mdns_publish.h`
- Modify: `clipsync-daemon/mdns_publish.c`
- Modify: `clipsync-daemon/clipsyncd.c`
- Modify: `clipsync-daemon/test_daemon_contract.ps1`

- [ ] **Step 1: Change mDNS interface to use the existing Mongoose manager**

Replace `clipsync-daemon/mdns_publish.h` with:

```c
#ifndef MDNS_PUBLISH_H
#define MDNS_PUBLISH_H

struct mg_mgr;

int mdns_publish_init(struct mg_mgr *mgr, int port, const char *instance_name);

#endif
```

- [ ] **Step 2: Implement DNS-SD response records**

Replace `clipsync-daemon/mdns_publish.c` with an implementation shaped like:

```c
#include "mdns_publish.h"
#include "protocol.h"
#include "mongoose.h"
#include <stdio.h>
#include <string.h>

static char g_instance[64] = "ClipSync Android";
static char g_txt_raw[] = "\x09version=1\x0aproto=json\x10auth=hmac-sha256";
static struct mg_dnssd_record g_record;
static struct mg_connection *g_mdns = NULL;

static void mdns_event_handler(struct mg_connection *c, int ev, void *ev_data) {
    (void)c;
    if (ev != MG_EV_MDNS_REQ) return;

    struct mg_mdns_req *req = (struct mg_mdns_req *) ev_data;
    if (req->is_listing ||
        mg_strcmp(req->reqname, mg_str(MDNS_SERVICE_TYPE ".local")) == 0 ||
        mg_strcmp(req->reqname, mg_str(MDNS_SERVICE_TYPE)) == 0) {
        req->r = &g_record;
        req->respname = mg_str(g_instance);
        req->is_resp = true;
    }
}

int mdns_publish_init(struct mg_mgr *mgr, int port, const char *instance_name) {
    if (!mgr) return -1;
    if (instance_name && instance_name[0] != '\0') {
        strncpy(g_instance, instance_name, sizeof(g_instance) - 1);
        g_instance[sizeof(g_instance) - 1] = '\0';
    }

    g_record.srvcproto = mg_str(MDNS_SERVICE_TYPE ".local");
    g_record.txt = mg_str_n(g_txt_raw, sizeof(g_txt_raw) - 1);
    g_record.port = (uint16_t) port;

    g_mdns = mg_mdns_listen(mgr, mdns_event_handler, g_instance);
    if (!g_mdns) {
        fprintf(stderr, "[mdns] failed to start mDNS listener\n");
        return -1;
    }

    printf("[mdns] publishing %s as '%s' on port %d\n", MDNS_SERVICE_TYPE ".local", g_instance, port);
    return 0;
}
```

If compile shows that Mongoose 7.21 expects `srvcproto` without `.local`, change only this line:

```c
g_record.srvcproto = mg_str(MDNS_SERVICE_TYPE);
```

Then re-run the PC discovery test in Task 7 before committing.

- [ ] **Step 3: Integrate from `clipsyncd.c`**

Replace:

```c
if (mdns_publish_init(port) != 0) {
```

with:

```c
if (mdns_publish_init(ws_server_mgr(), port, "ClipSync Android") != 0) {
```

Remove any remaining call to `mdns_publish_poll()`.

- [ ] **Step 4: Update contract test**

Add to `clipsync-daemon/test_daemon_contract.ps1`:

```powershell
$mdnsPublish = Get-Content -Raw (Join-Path $root "mdns_publish.c")

if ($mdnsPublish -notmatch "mg_mdns_listen") {
    throw "mdns_publish must register a real Mongoose mDNS listener"
}

if ($mdnsPublish -notmatch "_clipsync\\._tcp") {
    throw "mdns_publish must advertise _clipsync._tcp"
}
```

- [ ] **Step 5: Build**

```powershell
make build\clipsyncd
.\test_daemon_contract.ps1
```

Expected: daemon compiles and contract checks pass.

- [ ] **Step 6: Commit**

```powershell
git add clipsync-daemon/mdns_publish.* clipsync-daemon/clipsyncd.c clipsync-daemon/test_daemon_contract.ps1
git commit -m "feat: publish Android daemon over mDNS"
```

---

### Task 6: Build Real Clipboard Push JSON In `clipsyncd`

**Files:**
- Modify: `clipsync-daemon/clipsyncd.c`

- [ ] **Step 1: Replace unsafe `snprintf` JSON construction**

In `clipsync-daemon/clipsyncd.c`, add:

```c
#include "protocol_json.h"
```

Replace the manual JSON build in `on_clip_change()`:

```c
char json[65536 * 2];
unsigned long ts = (unsigned long)time(NULL) * 1000;
snprintf(json, sizeof(json),
    "{\"type\":\"clipboard_push\",\"text\":\"%s\",\"ts\":%lu}",
    text, ts);
ws_server_broadcast(json);
```

with:

```c
unsigned long long ts = (unsigned long long)time(NULL) * 1000ULL;
char *json = protocol_json_build_clipboard_push(text, ts);
if (json) {
    ws_server_broadcast(json);
    free(json);
}
```

- [ ] **Step 2: Keep echo prevention behavior**

Ensure `on_ws_set()` still updates `g_last_text` before calling `binder_clip_set_text(text)`, so the Binder callback does not immediately rebroadcast the same text:

```c
static void on_ws_set(const char *text) {
    if (!text) return;
    printf("[clipsyncd] received set: %lu chars\n", (unsigned long)strlen(text));
    strncpy(g_last_text, text, sizeof(g_last_text) - 1);
    g_last_text[sizeof(g_last_text) - 1] = '\0';
    binder_clip_set_text(text);
}
```

- [ ] **Step 3: Build**

```powershell
make build\clipsyncd
```

Expected: compile succeeds.

- [ ] **Step 4: Commit**

```powershell
git add clipsync-daemon/clipsyncd.c
git commit -m "fix: build daemon clipboard JSON through protocol helper"
```

---

### Task 7: Add PowerShell WebSocket Smoke Test

**Files:**
- Create: `clipsync-daemon/tools/ws_smoke.ps1`

- [ ] **Step 1: Create smoke test script**

Create `clipsync-daemon/tools/ws_smoke.ps1`:

```powershell
param(
    [Parameter(Mandatory=$true)]
    [string]$Uri,

    [string]$Secret = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function ConvertTo-HexLower([byte[]]$Bytes) {
    ($Bytes | ForEach-Object { $_.ToString("x2") }) -join ""
}

$ws = [System.Net.WebSockets.ClientWebSocket]::new()
$ct = [Threading.CancellationToken]::None
$ws.ConnectAsync([Uri]$Uri, $ct).GetAwaiter().GetResult()

$buffer = New-Object byte[] 4096
$segment = [ArraySegment[byte]]::new($buffer)
$result = $ws.ReceiveAsync($segment, $ct).GetAwaiter().GetResult()
$helloJson = [Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count)
$hello = $helloJson | ConvertFrom-Json

if ($hello.type -ne "hello") {
    throw "Expected hello, got: $helloJson"
}

$hmac = [System.Security.Cryptography.HMACSHA256]::new([Text.Encoding]::UTF8.GetBytes($Secret))
$digest = $hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes([string]$hello.challenge))
$response = ConvertTo-HexLower $digest
$authJson = @{ type = "auth"; response = $response } | ConvertTo-Json -Compress
$authBytes = [Text.Encoding]::UTF8.GetBytes($authJson)
$ws.SendAsync([ArraySegment[byte]]::new($authBytes), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).GetAwaiter().GetResult()

$result = $ws.ReceiveAsync($segment, $ct).GetAwaiter().GetResult()
$authResultJson = [Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count)
$authResult = $authResultJson | ConvertFrom-Json

if ($authResult.type -ne "auth_ok") {
    throw "Expected auth_ok, got: $authResultJson"
}

$pingJson = @{ type = "ping" } | ConvertTo-Json -Compress
$pingBytes = [Text.Encoding]::UTF8.GetBytes($pingJson)
$ws.SendAsync([ArraySegment[byte]]::new($pingBytes), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).GetAwaiter().GetResult()

$result = $ws.ReceiveAsync($segment, $ct).GetAwaiter().GetResult()
$pongJson = [Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count)
$pong = $pongJson | ConvertFrom-Json

if ($pong.type -ne "pong") {
    throw "Expected pong, got: $pongJson"
}

$ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, "done", $ct).GetAwaiter().GetResult()
Write-Host "websocket smoke passed: $Uri"
```

- [ ] **Step 2: Run against a manually started daemon**

After installing the daemon in Task 8, run:

```powershell
.\tools\ws_smoke.ps1 -Uri "ws://PHONE_IP:5287/ws" -Secret ""
```

Expected: `websocket smoke passed: ws://PHONE_IP:5287/ws`.

- [ ] **Step 3: Commit**

```powershell
git add clipsync-daemon/tools/ws_smoke.ps1
git commit -m "test: add daemon WebSocket smoke test"
```

---

### Task 8: Device Install And Network Verification

**Files:**
- No source edits.

- [ ] **Step 1: Build module**

```powershell
$env:ANDROID_NDK_ROOT = "D:\AppData\Android\sdk\ndk\30.0.14904198"
$env:PATH = "$env:ANDROID_NDK_ROOT;$env:ANDROID_NDK_ROOT\toolchains\llvm\prebuilt\windows-x86_64\bin;$env:ANDROID_NDK_ROOT\prebuilt\windows-x86_64\bin;$env:PATH"
$env:CC = "aarch64-linux-android33-clang"
make module
```

Expected: `clipsync-daemon/module/system/bin/clipsyncd` exists.

- [ ] **Step 2: Keep the Zygisk `.so` md5 check from `AGENTS.md`**

```powershell
Get-FileHash -Path clipsync-daemon\zygisk\libs\arm64-v8a\libzygisk_clipsync.so, `
               clipsync-daemon\module\zygisk\arm64-v8a.so -Algorithm MD5
```

Expected: hashes match. If they differ:

```powershell
Copy-Item -Path clipsync-daemon\zygisk\libs\arm64-v8a\libzygisk_clipsync.so `
          -Destination clipsync-daemon\module\zygisk\arm64-v8a.so -Force
```

- [ ] **Step 3: Install module**

```powershell
adb shell "su -c 'rm -rf /data/local/tmp/clipsyncd-module /data/adb/modules/clipsyncd'"
adb push clipsync-daemon/module/. /data/local/tmp/clipsyncd-module/
adb shell "su -c 'cp -r /data/local/tmp/clipsyncd-module /data/adb/modules/clipsyncd && chmod -R 0755 /data/adb/modules/clipsyncd'"
adb reboot
```

- [ ] **Step 4: Verify daemon process and TCP listener**

After boot:

```powershell
adb shell "su -c 'pidof clipsyncd || true'"
adb shell "su -c 'cat /proc/net/tcp /proc/net/tcp6 | grep -i :14A7 || echo no 5287 listener'"
```

Expected:

- `pidof clipsyncd` returns a PID.
- `/proc/net/tcp` or `/proc/net/tcp6` contains `:14A7`.

- [ ] **Step 5: Run WebSocket smoke**

Find phone IP:

```powershell
adb shell "ip addr show wlan0 | grep 'inet '"
```

Run:

```powershell
cd clipsync-daemon
.\tools\ws_smoke.ps1 -Uri "ws://PHONE_IP:5287/ws" -Secret ""
```

Expected: `websocket smoke passed`.

- [ ] **Step 6: Verify PC mDNS discovery path**

Run the PC client with logging:

```powershell
cd ..\clipsync-pc
$env:RUST_LOG = "info"
cargo run
```

Expected log contains:

```text
mDNS discovered: ws://PHONE_IP:5287
HMAC authentication successful
```

- [ ] **Step 7: Verify clipboard route**

With PC connected:

```powershell
adb shell "su -c 'logcat -c'"
```

Copy text on PC and wait two seconds. Then inspect daemon logs:

```powershell
adb shell "su -c 'logcat -d | grep -i clipsyncd'"
```

Expected:

- daemon logs a received WebSocket set.
- phone clipboard changes to the PC text.

Copy text on phone. Expected:

- daemon logs a pushed clipboard message.
- PC clipboard changes to the phone text.

---

### Task 9: Harden SELinux And Service Startup Only If Verification Requires It

**Files:**
- Modify only if needed: `clipsync-daemon/module/sepolicy.rule`
- Modify only if needed: `clipsync-daemon/module/service.sh`

- [ ] **Step 1: Check for denied socket operations**

```powershell
adb shell "su -c 'dmesg | grep -i denied | grep -E \"clipsyncd|ksu|system_server|socket\" || true'"
```

Expected: no denial involving `clipsyncd` network sockets.

- [ ] **Step 2: If daemon cannot bind/listen, add only the minimal observed allow rules**

Append to `clipsync-daemon/module/sepolicy.rule` only after an actual denial proves it is needed:

```text
allow ksu self:tcp_socket create_socket_perms;
allow ksu node:tcp_socket node_bind;
allow ksu port:tcp_socket name_bind;
allow ksu self:udp_socket create_socket_perms;
```

Then reinstall module and reboot.

- [ ] **Step 3: If mDNS cannot bind UDP 5353, verify with procfs**

```powershell
adb shell "su -c 'cat /proc/net/udp /proc/net/udp6 | grep -i :14E9 || echo no mdns udp socket'"
```

Expected: UDP port `5353` appears as `:14E9`.

- [ ] **Step 4: Commit only if changes were required**

```powershell
git add clipsync-daemon/module/sepolicy.rule clipsync-daemon/module/service.sh
git commit -m "fix: allow daemon network sockets under KernelSU policy"
```

---

## Acceptance Checklist

- [ ] `make build\clipsyncd` succeeds.
- [ ] `make test-crypto` passes on Android.
- [ ] `make test-json` passes on Android.
- [ ] `.\clipsync-daemon\test_daemon_contract.ps1` passes.
- [ ] `clipsyncd` starts from `/data/adb/modules/clipsyncd/system/bin/clipsyncd`.
- [ ] `/proc/net/tcp` or `/proc/net/tcp6` shows `:14A7`.
- [ ] `tools/ws_smoke.ps1` receives `hello`, sends HMAC auth, receives `auth_ok`, sends `ping`, receives `pong`.
- [ ] PC client discovers `_clipsync._tcp.local.` and connects without manual IP.
- [ ] PC-to-phone clipboard set calls Binder `setPrimaryClip`.
- [ ] Phone-to-PC clipboard change broadcasts JSON through WebSocket.
- [ ] Malformed text containing quotes, backslashes, and newlines transfers without broken JSON.

## Self-Review

- Spec coverage: Covers real WebSocket server, HMAC auth, mDNS/DNS-SD publish, integration into existing daemon, build packaging, and device verification.
- Placeholder scan: No implementation step defers required work. The one conditional mDNS `srvcproto` adjustment is tied to a compile/runtime check against Mongoose 7.21 behavior.
- Type consistency: New functions are named consistently across headers, implementations, tests, and call sites: `clipsync_hmac_sha256_hex`, `protocol_json_*`, `ws_server_mgr`, `mdns_publish_init`.
- Scope check: This plan intentionally does not fix Zygisk `system_server` injection or Binder parcel correctness unless those block the WebSocket/mDNS acceptance checks.
