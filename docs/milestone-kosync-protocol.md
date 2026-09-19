# Milestone: KOSync Protocol Core And Local Mock Server

**Status: HOST VALIDATION PASS; HARDWARE VALIDATION PASS.** CrossNook now
has a bounded KOSync progress client, the minimal required HTTP `GET`/`PUT`
transport, deterministic protocol fixtures, a standard-library Python mock
server, and a static ARM diagnostic CLI. This milestone is plain HTTP for a
controlled LAN only. It is not secure production or Internet synchronization.

## Upstream Pins And Sources

The protocol was researched from immutable sources, not memory or old protocol
descriptions:

| Component | Revision |
| --- | --- |
| `koreader/koreader` | `8da811b1bd4f33c2ce239885fe96e24771aba308` |
| `koreader/koreader-base` | `9a8729713ab73f539b607af23ede6aa89d04cfed` |
| KOReader-base CREngine gitlink | `55b42254511de90a56c740fd5df5979b5f921ddc` |
| CrossNook CREngine | `b05cf00791d2ba17c1f077fc69e7bd68cf780b6a` |

Exact files inspected and the behavior each defines:

| Source | Protocol behavior |
| --- | --- |
| `plugins/kosync.koplugin/main.lua` | identity selection, password hashing call site, position and percentage sources, device fields, push/pull flow, equality checks, timestamp conflict classification, and UI strategies |
| `plugins/kosync.koplugin/KOSyncClient.lua` | middleware, auth headers, vendor `Accept`, payload construction, `progress=tostring(progress)`, timeouts, and status handling |
| `plugins/kosync.koplugin/api.json` | base URL, endpoint paths, methods, required/optional payload fields, and expected statuses |
| `plugins/kosync.koplugin/KOSyncQueue.lua` | later retry behavior; inspected but explicitly not implemented here |
| `frontend/apps/reader/modules/readerrolling.lua` | reflowable-document XPointer and percentage values and XPointer restore |
| `frontend/apps/reader/modules/readerpaging.lua` | fixed-page document page-number progress and percentage |
| `frontend/document/credocument.lua` | CREngine `getXPointer`, `gotoXPointer`, and XPointer-to-position calls |
| `spec/unit/kosync_spec.lua` | protocol examples and authentication/payload expectations |
| `koreader-base/cre.cpp` | Lua binding of CREngine XPointer capture and restore |
| `koreader-base/thirdparty/lua-Spore/CMakeLists.txt` | lua-Spore `0.4.2`, commit `f106f930e4fd6110f714dd89f8886e868c74a583` |
| lua-Spore `src/Spore/Middleware/Format/JSON.lua` | JSON request/response middleware and `Content-Type` behavior |
| `koreader-base/thirdparty/dkjson/CMakeLists.txt` | pinned `dkjson 2.10` used by Spore |

Immutable primary client sources:

- <https://github.com/koreader/koreader/blob/8da811b1bd4f33c2ce239885fe96e24771aba308/plugins/kosync.koplugin/main.lua>
- <https://github.com/koreader/koreader/blob/8da811b1bd4f33c2ce239885fe96e24771aba308/plugins/kosync.koplugin/KOSyncClient.lua>
- <https://github.com/koreader/koreader/blob/8da811b1bd4f33c2ce239885fe96e24771aba308/plugins/kosync.koplugin/api.json>
- <https://github.com/koreader/koreader/blob/8da811b1bd4f33c2ce239885fe96e24771aba308/frontend/apps/reader/modules/readerrolling.lua>
- <https://github.com/koreader/koreader/blob/8da811b1bd4f33c2ce239885fe96e24771aba308/frontend/document/credocument.lua>
- <https://github.com/koreader/koreader-base/blob/9a8729713ab73f539b607af23ede6aa89d04cfed/cre.cpp>

KOReader does not pin a sync-server revision. To confirm server-generated
timestamp and overwrite behavior, the official server was additionally
inspected at `koreader-sync-server` commit
`237ab22943a206e74451176d709855c2b429eccf`, specifically
`app/controllers/1/syncs_controller.lua`, `config/routes.lua`, and
`spec/controllers/1/syncs_controller_spec.lua`. This is supplemental server
evidence, not a new client pin.

## KOReader-Compatible Protocol Behavior

### Endpoints And Methods

The service spec defines:

| Method | Path | Purpose |
| --- | --- | --- |
| `POST` | `/users/create` | register a username and password-derived key |
| `GET` | `/users/auth` | validate auth headers |
| `PUT` | `/syncs/progress` | create or replace one user's document progress |
| `GET` | `/syncs/progress/:document` | retrieve one user's document progress |

This milestone implements the two progress endpoints only. Registration and a
separate auth probe are unnecessary for deterministic tests because the mock
accepts an explicitly supplied runtime username and user key.

The default upstream base URL is `https://sync.koreader.rocks:443/`.
CrossNook instead requires an explicit `http://` base URL. `https://` is
rejected, not silently downgraded.

### Headers And Authentication

KOReader's client sets:

```text
Accept: application/vnd.koreader.v1+json
x-auth-user: <username>
x-auth-key: <userkey>
```

JSON requests also use `Content-Type: application/json`. There is no HTTP Basic
or Bearer authorization header.

KOReader computes `userkey = MD5(password)` as 32 lowercase hexadecimal bytes
before calling `KOSyncClient`, then stores and sends that key. CrossNook's
protocol API accepts the already-derived username and userkey explicitly and
does not persist either. The mock's dummy credentials are:

```text
username: test-user
password used to derive fixture key: test-password
userkey: dfb450efddbb5387197c84460623675b
```

No real credential may be used with this plain-HTTP milestone.

### PUT Request And Response

`PUT /syncs/progress` requires this JSON payload:

```json
{
  "document": "e1a1e9016cfc9bca8c694187943e9c4f",
  "progress": "/body/DocFragment[1]/body/p[1]/text().0",
  "percentage": 0.321,
  "device": "crossnook-test-device",
  "device_id": "crossnook-test-device-id"
}
```

`metadata` is an optional object in the pinned service spec. KOReader sends it
only when `send_metadata` is enabled; the default is disabled. CrossNook omits
it in this milestone.

The official server accepts an authenticated valid write unconditionally,
replacing the existing record for the same user and document. It does not
compare percentage, position, device timestamp, or a client timestamp. Its
successful response is:

```json
{
  "document": "e1a1e9016cfc9bca8c694187943e9c4f",
  "timestamp": 1700000000
}
```

The service spec lists `200`, `202`, and `401`, but pinned
`KOSyncClient:update_progress()` reports success only for `200`. CrossNook
matches that effective client behavior.

### GET Response

`GET /syncs/progress/:document` has no request body. A found record is:

```json
{
  "document": "e1a1e9016cfc9bca8c694187943e9c4f",
  "progress": "/body/DocFragment[1]/body/p[1]/text().0",
  "percentage": 0.321,
  "device": "crossnook-test-device",
  "device_id": "crossnook-test-device-id",
  "timestamp": 1700000000
}
```

An unknown document is still HTTP `200` with `{}`. It is not `404`. Progress is
scoped by both authenticated username and document ID.

### Document Identity

The `document` field is KOReader's selected Binary or Filename digest. Binary
is method `0` and the default. CrossNook uses the already validated lowercase
32-byte Binary partial-MD5 from `src/book/koreader_identity.*`.

Local Progress remains separate and unchanged:

```text
Local ProgressStore key       KOSync document field
path-v1-...                   KOReader Binary partial-MD5
```

`path-v1-*`, paths, and filenames are never added to the KOSync wire object.

The golden IDs are:

| Fixture | Binary document ID |
| --- | --- |
| `test.epub` | `e1a1e9016cfc9bca8c694187943e9c4f` |
| `valid2.epub` | `e1a1e9016cfc9bca8c694187943e9c4f` |
| `foreign.epub` | `519220cea448409961e6b3081a36eca3` |

`test.epub` and `valid2.epub` intentionally collide under the sampled Binary
algorithm. KOReader and CrossNook therefore address them as the same KOSync
document for one user. A PUT through either identity replaces the other's
remote record. This is a compatibility property, not an error, and is not
"fixed" by mixing a path or filename into Binary identity. Independent mock
tests use `foreign.epub` or another distinct 32-byte fixture ID.

### Position Semantics

KOSync sends both position and percentage.

For reflowable CREngine documents, `ReaderRolling:getLastProgress()` returns
its current XPointer. `KOSyncClient` applies `tostring()` and JSON encoding. No
URL encoding, Base64, or XPointer-specific transformation occurs. JSON escaping
is the only escaping layer.

CrossNook captures `LVDocView::getBookmark(true).toString()` and converts it to
UTF-8. Pinned KOReader's CRE binding captures `getBookmark()`; its default
argument is also `precise=true`, and it serializes the same CREngine XPointer.
The relevant capture/parser/serializer code is unchanged between KOReader's
CREngine `55b42254...` and CrossNook's `b05cf007...`. Current-DOM EPUB tokens
are therefore wire-compatible and are copied unchanged by
`cn_kosync_progress_set()`.

Compatibility boundary: KOReader may preserve an older per-book
`cre_dom_version`. Older normalized spelling can omit explicit `[1]` indexes,
and still older V1 XPointers can include different DOM boxing details.
CrossNook's Reader deliberately requires exact current canonical
reserialization, so an old spelling that resolves but canonicalizes differently
is rejected. No guessed conversion is introduced in this milestone.

Pinned KOReader restore emits `GotoXPointer` and lets CREngine resolve the
remote string. A normal font/margin relayout retains the XPointer location. An
unresolved value can fall back to the first page/top in KOReader's current
path. Future CrossNook UI integration must instead pass the token through the
existing `cn_reader_goto_position()` validation, which rejects unresolved or
noncanonical input without moving. The protocol client itself does not move
the Reader.

For fixed-page documents, KOReader sends a page number string. CrossNook's
current Reader supports reflowable EPUB logical XPointers; no physical-page
wire mode is added here.

### Percentage Semantics

The wire `percentage` is a JSON number from `0` through `1`. KOReader truncates
it to four decimal places with `Math.roundPercent()` before sending and after
receiving.

For a reflowable document in KOReader page view, KOReader derives it from
`current_page / number_of_pages`; in scroll view it derives it from the
XPointer's vertical document coordinate divided by document height. CrossNook's
existing `ReaderPosition.progress_10000` is the latter normalized logical
coordinate in integer `0..10000` units. The sync-domain API serializes that
value exactly as a trimmed four-decimal `0..1` number. This avoids inventing a
physical-page conversion and preserves CrossNook's existing normalized
metadata, but percentages from CrossNook and KOReader page view may differ
slightly even when their canonical XPointer is equal. The XPointer remains the
canonical location.

### Timestamp And Update Semantics

No timestamp is present in the PUT request. The official server assigns
`os.time()` on each accepted write and returns/stores that Unix wall-clock
value in **seconds**, not milliseconds.

The mock starts at an injected deterministic integer and increments it once per
accepted PUT. Host and hardware tests therefore do not depend on PC or Nook
wall-clock accuracy. Production synchronization will need a deliberate clock
policy because pinned KOReader compares server seconds against the device's
local `os.time()`.

### Device Semantics

KOReader sends:

- `device`: the configured KOSync hostname or `Device.model`;
- `device_id`: the global KOReader `device_id` setting.

Both are UTF-8 JSON strings. CrossNook requires explicit nonempty runtime
values of at most 255 bytes and does not derive a permanent identity, use a MAC
address, or write an identity to eMMC. Tests use `crossnook-test-device` and
`crossnook-test-device-id`.

### Conflict Classification

The server does not resolve conflicts; it stores the last accepted PUT.
Pinned KOReader pull behavior is:

1. Ignore a result whose `device` matches `Device.model` and whose `device_id`
   matches the local ID. Notably, pinned KOReader still compares against
   `Device.model` when a custom upload hostname was used.
2. Treat it as already synchronized when either the four-decimal percentage is
   equal or the `progress` string is equal.
3. For an interactive pull, apply the remaining remote progress without a
   timestamp-direction prompt.
4. For automatic pull, a present remote timestamp is newer only when it is
   strictly greater than the local last-page-turn `os.time()` value.
5. Equal timestamps follow the older/equal branch.
6. With an old server that omits timestamp, use remote percentage greater than
   local percentage as the newer test.
7. KOReader's default future/newer policy is prompt; its default older policy
   is disabled.

`cn_kosync_classify_remote()` implements only facts 1, 2, 4, 5, and 6 as a pure
classification. It does not apply a position or choose prompt/silent/disabled.
Those remain future CrossNook product/UI policy.

## CrossNook Architecture

```text
CREngine
   |
ReaderPosition ----------------------+
   |                                 |
ProgressStore (path-v1, unchanged)   |
                                     v
BookIdentity (KOReader Binary) -> cn_kosync_progress
                                     |
                                     v
                              KOSync protocol core
                                     |
                                     v
                              netsimple HTTP transport
```

`src/sync/kosync.{h,c}` owns endpoint paths, auth and vendor headers, JSON,
wire validation, response interpretation, and remote-state classification.
Reader/UI code does not construct JSON or HTTP. ProgressStore has no KOSync
wire knowledge. `netsimple` has no KOSync field knowledge.

Public operations are:

```c
cn_kosync_client_init(... explicit http:// base URL and runtime credentials)
cn_kosync_progress_set(... document, logical position, normalized progress,
                       device, device_id)
cn_kosync_put_progress(...)
cn_kosync_get_progress(...)
cn_kosync_classify_remote(...)
```

## JSON Choice And Bounds

No suitable JSON implementation already existed in CrossNook. Adding a large
dependency for two flat objects was unnecessary, so `kosync.c` contains a
schema-specific iterative codec rather than a general recursive DOM parser.
It:

- accepts only a top-level object and scalar fields;
- has no recursion;
- bounds wire JSON and decoded logical position;
- rejects truncation, malformed syntax, invalid escapes, invalid Unicode
  surrogate pairs, embedded NUL, invalid UTF-8, duplicate known fields, and
  malformed protocol types;
- decodes all JSON string escapes, including surrogate pairs;
- escapes arbitrary strings through a dedicated byte-wise encoder rather than
  `sprintf`; and
- allocates only bounded buffers.

The logical position limit matches `CN_READER_POSITION_MAX_BYTES` at 65,536
bytes. Worst-case escaped JSON is bounded by `CN_KOSYNC_JSON_MAX`.

## HTTP Transport Changes

`src/net/netsimple.*` retains `cn_netsimple_get()` and its existing result
names. The new narrow `cn_netsimple_exchange()` supports only what this pinned
protocol needs:

- `GET` and `PUT`;
- a caller-owned request body with explicit byte length;
- up to eight validated custom headers;
- `Content-Type` and `Content-Length`; and
- caller-selected bounded response capacity.

It remains IPv4-only, HTTP/1.0, connection-close delimited, and free of
TLS/libcurl. Connect, send, and receive socket I/O have monotonic deadlines;
system hostname resolution remains subject to the platform resolver. Header
names, values, hosts, and request targets reject control characters; bodies are
sent with nonblocking short-write handling; sockets close on every path; and
existing connect error names are unchanged.

## Deterministic Mock

`testapp/kosync_mock_server.py` uses Python's standard library only. It exposes
the exact two progress paths, validates the vendor/auth/content headers and
payload schema, scopes records by user plus document, returns `{}` for a miss,
replaces a same-document record unconditionally, and assigns injected
second-based timestamps.

Its self-test covers malformed requests, auth failure, missing progress, first
upload, retrieval, replacement, distinct documents, and distinct users. Four
reserved document IDs provide explicit test-only responses for malformed JSON,
malformed protocol fields, oversized response, and HTTP 500. They are mock
fault controls, not claimed KOSync service behavior.

Golden values are in `testapp/kosync-fixtures.json`. The file records all three
validated EPUB IDs and explicitly records the `test.epub`/`valid2.epub`
collision.

## Host Validation

Run:

```sh
bash testapp/build-kosync.sh
bash testapp/build-nettest.sh
bash testapp/build-bookid.sh
bash testapp/build-position.sh
bash testapp/build-progress.sh
```

The KOSync suite validates the Python mock itself, builds a static ARM EABI5
soft-float non-PIE binary, and runs it under qemu-arm. It covers exact JSON and
auth/header serialization, all golden IDs, XPointer and escape preservation,
four-decimal percentage, second timestamps, `GET`, `PUT`, missing progress,
same-document replacement, distinct documents/users, malformed request and
response JSON, malformed protocol fields, 401, 500, network failure,
response-size limit, and pinned timestamp/equality classification.

Observed result:

```text
KOSYNC HOST VALIDATION OK
```

These tests validate protocol behavior and the host TCP path. They do not
validate Nook Wi-Fi or hardware execution.

## Real-Nook Hardware Validation

The freshly built static ARM diagnostic client passed on a physical Nook Simple
Touch over the previously hardware-validated Wi-Fi/networking stack. The peer
was the deterministic plain-HTTP mock KOSync server on the Windows PC.

Exact device output:

```text
[OK] first progress upload
[OK] first progress retrieval
[OK] same-document update
[OK] updated progress retrieval
[OK] distinct-document upload
[OK] distinct documents remain independent
KOSYNC ROUNDTRIP failures=0 first=1700000000 update=1700000001 other=1700000002 -> OK
```

This proves on Linux 2.6.29 / ARMv7 hardware that request, authentication/header,
document ID, logical position, and progress serialization are accepted by the
mock; `PUT` and `GET` traverse the real Nook networking stack; same-document
replacement and distinct-document isolation work; and responses are parsed
back into CrossNook successfully. The observed timestamps were the mock's
deterministic sequence: first `1700000000`, update `1700000001`, and other
`1700000002`.

No real KOSync credentials were used. The test used only the controlled LAN
plain-HTTP mock endpoint and made no writes to `/data`, `/system`, or `/rom`.

### Reproduction Procedure

Run PowerShell as needed for the firewall command and use the repository root
as the working directory.

1. Build and start the deterministic LAN mock on the Windows PC:

   ```powershell
   bash testapp/build-kosync.sh
   python testapp\kosync_mock_server.py --host 0.0.0.0 --port 8000 --timestamp-start 1700000000
   ```

2. In an elevated PowerShell window, permit only Private-network TCP/8000:

   ```powershell
   New-NetFirewallRule -DisplayName crossnook-kosync-tcp8000 -Direction Inbound `
     -Protocol TCP -LocalPort 8000 -Action Allow -Profile Private
   ```

3. Prepare only `/tmp` on the Nook and push the diagnostic binary:

   ```powershell
   C:\platform-tools\adb.exe devices
   C:\platform-tools\adb.exe shell "rm -rf /tmp/crossnook-kosync; mkdir -p /tmp/crossnook-kosync"
   C:\platform-tools\adb.exe push testapp\crossnook-kosync-test /tmp/crossnook-kosync/
   C:\platform-tools\adb.exe shell "chmod 755 /tmp/crossnook-kosync/crossnook-kosync-test"
   ```

4. Create `C:\temp\crossnook-wifi.conf` outside the repository with the test
   AP credentials, push it to `/tmp`, and immediately remove the PC copy:

   ```powershell
   C:\platform-tools\adb.exe push C:\temp\crossnook-wifi.conf /tmp/crossnook-wifi.conf
   Remove-Item C:\temp\crossnook-wifi.conf
   ```

5. Use the already validated Wi-Fi flow. Loading the driver triggers stock
   services, so stop them before the manual `/tmp` configuration owns the
   interface:

   ```powershell
   C:\platform-tools\adb.exe shell "insmod /etc/wifi/tiwlan_drv.ko"
   C:\platform-tools\adb.exe shell "stop wpa_supplicant; stop ifcfg_ti; stop dhcpcd; ifconfig tiwlan0 up"
   ```

6. In one PowerShell window, keep the measured supplicant command running in
   the foreground until it prints `CTRL-EVENT-CONNECTED`:

   ```powershell
   C:\platform-tools\adb.exe shell "/sbin/wpa_supplicant -Dtiwlan0 -itiwlan0 -c/tmp/crossnook-wifi.conf -dd"
   ```

7. In a second PowerShell window, obtain IPv4/default route with the stock
   reference DHCP command and confirm `tiwlan0` plus `/proc/net/route`:

   ```powershell
   C:\platform-tools\adb.exe shell "/sbin/dhcpcd -ABKL -d tiwlan0"
   C:\platform-tools\adb.exe shell "ifconfig tiwlan0; cat /proc/net/route"
   ```

8. Replace `<PC-LAN-IP>` below with the PC LAN IPv4. Upload the first known
   document and logical position:

   ```powershell
   C:\platform-tools\adb.exe shell "/tmp/crossnook-kosync/crossnook-kosync-test --put http://<PC-LAN-IP>:8000 test-user dfb450efddbb5387197c84460623675b e1a1e9016cfc9bca8c694187943e9c4f '/body/DocFragment[1]/body/p[1]/text().0' 3210 crossnook-test-device crossnook-test-device-id"
   ```

9. Run a second process to retrieve it exactly:

   ```powershell
   C:\platform-tools\adb.exe shell "/tmp/crossnook-kosync/crossnook-kosync-test --get http://<PC-LAN-IP>:8000 test-user dfb450efddbb5387197c84460623675b e1a1e9016cfc9bca8c694187943e9c4f"
   ```

   Confirm document ID, `3210`, device fields, and XPointer are unchanged.

10. Upload a newer same-document value, then retrieve it in another process:

    ```powershell
    C:\platform-tools\adb.exe shell "/tmp/crossnook-kosync/crossnook-kosync-test --put http://<PC-LAN-IP>:8000 test-user dfb450efddbb5387197c84460623675b e1a1e9016cfc9bca8c694187943e9c4f '/body/DocFragment[1]/body/p[3]/text().5' 6543 crossnook-test-device crossnook-test-device-id"
    C:\platform-tools\adb.exe shell "/tmp/crossnook-kosync/crossnook-kosync-test --get http://<PC-LAN-IP>:8000 test-user dfb450efddbb5387197c84460623675b e1a1e9016cfc9bca8c694187943e9c4f"
    ```

11. Prove a distinct Binary ID remains independent:

    ```powershell
    C:\platform-tools\adb.exe shell "/tmp/crossnook-kosync/crossnook-kosync-test --put http://<PC-LAN-IP>:8000 test-user dfb450efddbb5387197c84460623675b 519220cea448409961e6b3081a36eca3 '/body/DocFragment[1]/body/p[1]/text().0' 1111 crossnook-test-device crossnook-test-device-id"
    C:\platform-tools\adb.exe shell "/tmp/crossnook-kosync/crossnook-kosync-test --get http://<PC-LAN-IP>:8000 test-user dfb450efddbb5387197c84460623675b 519220cea448409961e6b3081a36eca3"
    C:\platform-tools\adb.exe shell "/tmp/crossnook-kosync/crossnook-kosync-test --get http://<PC-LAN-IP>:8000 test-user dfb450efddbb5387197c84460623675b e1a1e9016cfc9bca8c694187943e9c4f"
    ```

12. The equivalent one-command diagnostic is:

    ```powershell
    C:\platform-tools\adb.exe shell "/tmp/crossnook-kosync/crossnook-kosync-test --roundtrip http://<PC-LAN-IP>:8000 test-user dfb450efddbb5387197c84460623675b e1a1e9016cfc9bca8c694187943e9c4f 519220cea448409961e6b3081a36eca3"
    ```

    Required final line:

    ```text
    KOSYNC ROUNDTRIP failures=0 ... -> OK
    ```

All runtime files and Wi-Fi credentials remain under `/tmp`. `/data`,
`/system`, and `/rom` are not written. `/rom/devconf/WiFiBackupCalibration` is
used read-only by the validated Wi-Fi loader path.

## Future CrossNook Product/UI Policy

This milestone intentionally stops before deciding when to sync, whether to
prompt, whether older progress may be applied, or how to present conflicts.
Future UI composition must use the protocol classification and the existing
Reader validation rather than putting JSON, HTTP, or physical-page logic in UI
code.

## Limitations And Remaining Work

- Plain HTTP exposes credentials and progress; use dummy LAN-only credentials.
- No TLS, HTTPS, certificate validation, or real Internet account.
- This milestone validates protocol compatibility only over controlled LAN
  plain HTTP; it does not validate production transport security.
- Production HTTPS/TLS and real Internet KOSync credentials or servers are not
  implemented or validated.
- No registration/settings UI or persistent credentials/device ID.
- No automatic/background/startup/exit/page-turn sync.
- No automatic Reader/UI sync integration.
- No retry or offline queue.
- No Wi-Fi reconnect or clock synchronization.
- No metadata upload.
- No old-DOM XPointer spelling migration.
- No Reader/UI application of downloaded progress yet.
- No ProgressStore, ReaderPosition, BookIdentity, or local identity migration.
- Production sync requires TLS and an explicit wall-clock/conflict policy.

The exact physical-device round trip above completes the host and hardware
validation scope for this protocol-core milestone.
