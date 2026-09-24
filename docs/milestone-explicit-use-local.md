# Explicit Use Local / Force Upload

**Status: IMPLEMENTED; HOST VALIDATION PASS; HARDWARE VALIDATION PASS; RELEASE READY.**

## Separately authorized operation

“Use local progress” explicitly makes an **already-persisted** ProgressStore
record authoritative on the KOSync server. `cn_sync_push_local_current_book()`
loads Settings/Credentials and invokes `cn_kosync_push_local_once()`; neither
function calls normal sync, remote pull, or Reader. The lower primitive derives
the exact-path local identity and KOReader Binary document identity, loads the
local record, converts it with `cn_kosync_progress_set()`, establishes trusted
time, resolves DNS, configures verified HTTPS, and calls the published
`cn_kosync_put_progress()` **once**. There is **no GET** or automatic retry.
The same authorized PUT is performed when remote state differs, is missing, or
already equals local state. A missing remote record is created by the PUT.

No timestamp, percentage, or XPointer ordering is used to infer freshness.
The prior conflict's remote payload is never consulted. Normal
`cn_sync_current_book()` retains its conservative CONFLICT behavior for
different positions; `src/sync/sync_controller.c`, `src/sync/kosync_sync.c`,
`src/sync/kosync_policy.c`, `src/sync/kosync.[ch]`, and the published Reader
Sync path are unchanged.

The ProgressStore format accepts percentage `-1` for unknown, but the KOSync
upload representation requires `0..10000`: `-1` returns LOCAL_UNSUPPORTED
**before network**. Missing, corrupt, unreadable or unuploadable local records
also stop before time/DNS/PUT. The existing KOSync conversion and serializer
enforce the 65,536-byte maximum logical position and required UTF-8, device,
and percentage rules; no second serializer was introduced. Settings provide
the device name (default `CrossNook`); the caller supplies `device_id` at
runtime. No ID is generated or persisted.

## Bounded result and mutation evidence

`cn_kosync_push_result` and `cn_sync_push_result` return bounded stages,
outcomes, identity/load/time/DNS/KOSync/transport/HTTP evidence and a
`put_invoked` flag. No credential, document path, XPointer or request body is
returned. `put_invoked` means entry into the existing PUT primitive; it does
not claim HTTP bytes were transmitted. The sync push operation makes **no
local writes**, so local mutation is always NONE.

Remote mutation is NONE before PUT invocation, including disabled, invalid
storage/app setup, missing credentials, local load/conversion failure, time or
DNS failure. A strict allowlist of connect and TLS-open failures that happen
before HTTP request transmission also returns NONE after an invocation.
Only a fully validated PUT acknowledgement yields CONFIRMED. All other errors
after PUT invocation conservatively yield POSSIBLE, including send failures,
auth/HTTP errors, receive timeout, truncated or malformed/mismatched
acknowledgements and post-send allocation failures. A server may have applied
the request before its response was lost. The diagnostic deliberately models
that case and requires POSSIBLE **without retry**. `cn_kosync_put_progress()`
exposes no exact bytes-sent flag, so unproven cases never claim NONE.

## Application composition and storage boundary

For a future user-facing action while the book is open: verify external
storage, capture Reader A with `cn_ui_reader_get_position()`, save A to the
verified ProgressStore, and **only after the save succeeds** call explicit
push. The push reloads the persisted A and leaves Reader at A. Do not call
`cn_reader_sync_manual_once()` (it runs normal sync) or
`cn_reader_sync_apply_persisted()` (it imports remote progress). The diagnostic
exercises capture/save/push with existing public APIs; there is no UI yet.

The lower sync API receives an already-open ProgressStore and cannot verify
the device to which it is rooted. Before *any* persistence or network use on
hardware, the app/diagnostic must call `cn_platform_storage_verify()` for the
external whole-device microSD (`/dev/block/mmcblk1`, `179:16`) mounted at
`/tmp/crossnook-card`, confirm the root
`/tmp/crossnook-card/crossnook`, then derive/check existing directories and
open the stores. Never use `/data` or intentionally persist to internal eMMC.
Synthetic credentials only; FAT32 mode bits provide no meaningful Unix
permission confidentiality. ProgressStore cannot report DURABILITY_UNCERTAIN;
FAT32 power-loss persistence is not guaranteed by an OK save.

## Focused host and regression gates

`bash testapp/build-local-push.sh` builds a static ARMv5TE EABI5 soft-float
non-PIE diagnostic and runs it under QEMU with fake network boundaries and
real ProgressStore/Reader. It covers local missing/corrupt/unknown percentage,
preflight and device-ID failures, remote overwrite/create/equal PUT, failure
classes before and after send, applied PUT with lost response, and real
Reader capture/save/push composition, including an unsaved Reader B that must
not displace persisted A. Counters and structural object checks
require the PUT path and prohibit GET and normal-sync references in the push
objects. Also run the published normal Sync Controller, Manual Reader Sync,
and Explicit Remote Pull host gates after the additive header change.

```text
LOCAL PUSH SMOKE failures=0 -> OK
LOCAL PUSH HOST VALIDATION OK
SYNC CONTROLLER HOST VALIDATION OK
READER SYNC HOST VALIDATION OK
REMOTE PULL HOST VALIDATION OK
```

The produced `testapp/crossnook-local-push-test` artifact has SHA-256
`b44d8a4bb1ec0bbfe65c51e78d231ca6340ef22b25401b5e98cd8e07c37251cd`.

## Physical validation

Physical validation passed on Barnes & Noble Nook Simple Touch / ARMv7 Linux
2.6.29 using the external whole-device microSD `/dev/block/mmcblk1`. In this
NookManager environment `/data` is internal eMMC and was not used for tests or
state. Runtime diagnostics used synthetic credentials and bounded status lines;
raw credential-bearing transcript content was not exposed.

The initial unmounted gate failed closed before persistence or network and
returned direct remote-shell status `REMOTE_RC=1`:

```text
LOCAL PUSH GATE storage=unverified persistence=not-attempted network=not-attempted
```

The outside-root sentinel remained `local-push-sentinel`. The external card was
then mounted at `/tmp/crossnook-card`, device identity `179:16`, and the
published verifier accepted `/tmp/crossnook-card/crossnook`:

```text
STORAGE VERIFY result=ok errno=0 root=/tmp/crossnook-card/crossnook mount=/tmp/crossnook-card device=179:16
```

### Trusted time observation

Two early physical push attempts failed before PUT with trusted time
unavailable:

```text
LOCAL PUSH GATE outcome=trusted-time-unavailable stage=executed put-invoked=0 local-load=ok local-mutation=none remote-mutation=none local-unchanged=yes reader-unchanged=yes
```

No HTTP request was added by those failed attempts. A standalone trusted-time
diagnostic then proved the component path worked:

```text
TIMETEST QUERY OK unix=1790278248 nsec=579040535 rtt_ms=1159 server=0
TIMETEST SYNC OK unix=1790278248 nsec=712341304 rtt_ms=1425 server=0
```

The system date became `Thu Sep 24 19:30:48 UTC 2026`. Physical Local Push
passes zero `timeout_ms`, intentionally selecting `CN_TIME_DEFAULT_TIMEOUT_MS`
of 3000 ms, and `CN_KOSYNC_SYNC_TIME_ESTABLISH` unconditionally invokes
`cn_timesimple_sync()` before DNS or PUT. Five later 3000 ms queries succeeded
with RTTs 118 ms, 4 ms, 4 ms, 6 ms, and 6 ms. A subsequent Local Push also
passed its own trusted-time path and uploaded. These two initial failures are
recorded as transient physical-test/network/mock failures; no reproducible
configuration or implementation defect was established.

### Remote differs

Local A and remote B were created in separate setup phases:

```text
LOCAL PUSH GATE seed-local=ok
LOCAL PUSH GATE seed-remote=ok setup-put=1
```

The measured explicit action performed no GET and one PUT:

```text
LOCAL PUSH GATE outcome=uploaded stage=executed put-invoked=1 local-load=ok local-mutation=none remote-mutation=confirmed local-unchanged=yes reader-unchanged=yes
```

Action delta:

```text
GET=0
PUT=1
```

The separate verification request was deliberately outside the action delta:

```text
LOCAL PUSH GATE verify-remote=ok reader-unchanged=yes network=GET-only
GET=1
PUT=0
```

The sentinel remained unchanged. This proves persisted local A overwrote
remote B through the explicit PUT-only action while Reader stayed unchanged.

### Remote already equal

Remote A was established first, then independently verified:

```text
LOCAL PUSH GATE verify-remote=ok reader-unchanged=yes network=GET-only
```

Baseline before the measured equal action was GET=1, PUT=1. The measured
action still performed exactly one unconditional PUT and no GET:

```text
LOCAL PUSH GATE outcome=uploaded stage=executed put-invoked=1 local-load=ok local-mutation=none remote-mutation=confirmed local-unchanged=yes reader-unchanged=yes
GET=0
PUT=1
```

The sentinel remained unchanged.

### Remote missing

A fresh HTTPS mock/transcript started with no remote state. Local A was seeded:

```text
LOCAL PUSH GATE seed-local=ok
```

Baseline was GET=0, PUT=0. The measured action created remote A by direct PUT:

```text
LOCAL PUSH GATE outcome=uploaded stage=executed put-invoked=1 local-load=ok local-mutation=none remote-mutation=confirmed local-unchanged=yes reader-unchanged=yes
GET=0
PUT=1
```

Separate verification confirmed A and was not part of the measured action:

```text
LOCAL PUSH GATE verify-remote=ok reader-unchanged=yes network=GET-only
GET=1
PUT=0
```

The sentinel remained unchanged.

### Unsupported local

The diagnostic saved a local record whose progress percentage was unknown:

```text
LOCAL PUSH GATE seed-unsupported=ok
```

The measured action stopped before network:

```text
LOCAL PUSH GATE outcome=local-unsupported stage=executed put-invoked=0 local-load=ok local-mutation=none remote-mutation=none local-unchanged=yes reader-unchanged=yes
GET=0
PUT=0
```

The sentinel remained unchanged. This proves `progress_10000 == -1` is not
uploaded.

### Missing local

A genuinely unused exact lexical EPUB path was created with the same EPUB
bytes but a new `/tmp` pathname, so no ProgressStore record existed for that
exact path. The first action returned:

```text
LOCAL PUSH GATE outcome=local-missing stage=executed put-invoked=0 local-load=missing local-mutation=none remote-mutation=none local-unchanged=no reader-unchanged=yes
GET=0
PUT=0
```

The same exact-path action was repeated and again returned `outcome=local-missing`,
`put-invoked=0`, `local-load=missing`, `local-mutation=none`, and
`remote-mutation=none`, with delta GET=0 and PUT=0. Therefore the first
attempt did not create or mutate local state. In this missing-local case,
`local-unchanged=no` is a diagnostic calculation artifact: the physical
diagnostic defines `local_equal` in terms of an existing pre-operation
persisted position, which is necessarily absent. It is not evidence of an
actual local mutation.

### Final unmount gate

After validation, unmount returned `UNMOUNT_RC=0` and `CARD_NOT_MOUNTED`.
The post-unmount action again failed closed before persistence or network:

```text
LOCAL PUSH GATE storage=unverified persistence=not-attempted network=not-attempted
REMOTE_RC=1
GET=0
PUT=0
```

The sentinel was still `local-push-sentinel`. This proves the app/physical
gate refuses operation after verified external storage disappears.

No CAS/revision protection exists. Another client may write after an
acknowledged PUT. UI authorization, stable persisted device identity, network
and mount lifecycle ownership, retry policy, and background sync are deferred.
