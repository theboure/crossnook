# Explicit Remote Pull

**Status: IMPLEMENTATION COMPLETE; HOST VALIDATION PASS; HARDWARE VALIDATION PASS; RELEASE READY.**

## Separate, explicitly destructive local choice

`cn_sync_pull_remote_current_book(const cn_sync_controller_config *config)`
is a separately named synchronous operation for a future, explicitly selected
“Use remote progress” action. It does **not** call or change published
`cn_sync_current_book()` or save-first Manual Reader Sync. No mode flag was
added to normal sync. The normal path retains UPLOADED / UNCHANGED / CONFLICT
semantics without guessing freshness from percentage or timestamp.

The new lower-level `cn_kosync_pull_remote_once(config, result)` lives in
`src/sync/kosync_pull.[ch]`. This primitive derives the local exact-path
ProgressStore identity and the KOReader Binary document hash; uses existing
trusted-time, DNS and verified TLS configuration; and invokes **one fresh**
`cn_kosync_get_progress()` over HTTPS. It never calls normal sync or PUT. The
original service hostname is retained for Host, SNI, and certificate checks
while the socket uses the numeric DNS answer. HTTP 200 with `{}` means
`REMOTE_MISSING`, whereas HTTP 404 is an HTTP/service error. No cached conflict
payload or remote XPointer can be passed to the API, so B observed during a
prior conflict is never silently reused if the server now returns C.

If the fresh GET returns no progress, do nothing to local ProgressStore or
Reader. If the returned remote position exists, save its owned logical
position and progress_10000 using the **existing** `cn_progress_record` format
and the exact lexical document path's local identity. This intentionally
overwrites local A regardless of equality, difference, or previous corruption:
the user authorization is the named operation, **not** a freshness comparison.
Remote timestamp, device and device ID are not persisted. The temporary
ProgressStore record borrows the GET-owned location only for the synchronous
save, detaches it before clear, and then clears the owning KOSync progress.

The new `src/sync/sync_pull_controller.c` performs Settings -> Credentials ->
HTTPS client -> lower-level pull. Settings missing/default-disabled or
explicitly disabled stop before credentials, identity, time, DNS or network.
Corrupt/unsupported Settings and Credentials retain bounded results distinct
from missing credentials. Credential/client copies are cleared after use.
`device_id` is **not** required for GET and is neither generated nor stored.
Normal `src/sync/sync_controller.c` is byte-for-byte unchanged, including its
existing device-ID requirement. The only published-header change is the
additive new pull API and result types in `sync_controller.h`.

## Bounded result and mutation evidence

`cn_sync_pull_result` includes pull-specific outcome, stage, Settings and
Credential results, the bounded identity/time/DNS/KOSync/transport/HTTP and
local-save results, GET/save attempted flags, and local/remote mutation state.
It owns no document path, credentials, network body or XPointer. Outcomes are
DISABLED, REMOTE_MISSING, PERSISTED, AUTH_REQUIRED, TRUSTED_TIME_UNAVAILABLE,
CONNECTIVITY_FAILURE, SECURITY_FAILURE, SERVICE_FAILURE, LOCAL_FAILURE,
CONFIGURATION_FAILURE and INTERNAL_FAILURE. It does not invent normal-sync
UPLOADED, NO_STATE, UNCHANGED or CONFLICT outcomes.

Remote mutation is always NONE. Local mutation is NONE before save or on
REMOTE_MISSING; CONFIRMED when ProgressStore reports OK; conservatively
POSSIBLE when save was attempted but reports failure. The current store's
pre-rename failure normally preserves the prior final record; no adversarial
mount replacement or power-loss certainty is claimed. ProgressStore cannot
report DURABILITY_UNCERTAIN: its successful result does **not** guarantee
FAT32 power-loss persistence. No retry loop or automatic conflict resolution
is present. Remote may change again after GET and before local save; the
service exposes no CAS/version guard.

The controller needs only the document path and already-open ProgressStore,
not a running Reader. If the selected book is open, a future application UI
may call published `cn_reader_sync_apply_persisted(ui, store)` **only after**
PERSISTED to reload the authoritative position and apply it, then separately
render/flush. If Reader application fails, disk may contain B while Reader
stays at A; a later close-save could overwrite B unless the future UI handles
that mismatch. No UI, persistent device ID, Wi-Fi/mount management or “Use
local” force-upload action is included.

## Focused host gate

`bash testapp/build-remote-pull.sh` cross-compiles a static ARMv5TE EABI5
soft-float non-PIE artifact and runs under QEMU. Link-time fake Settings/
Credential loads, time, DNS and GET prevent host network activity. Real
ProgressStore and Reader/EPUB fixtures test local A preservation, authorized
B/C replacement from fresh GET, missing and corrupt local records, preflight
and local-save failures, repeated calls, and Reader application of an already
persisted Reader-generated B.
Statuses for 401, trusted time, DNS, TLS hostname/trust, malformed/bad-protocol
responses, 404 and receive timeout remain distinct. Structural `nm` checks
prove the pull object references GET but not PUT; fake PUT and normal-sync call
counters must stay zero for all pull scenarios. Output/documentation redaction
checks forbid credential values. Published normal controller and Reader Sync
focused gates are also required after this additive controller-header change.

```text
REMOTE PULL SMOKE failures=0 -> OK
REMOTE PULL HOST VALIDATION OK
```

The produced `testapp/crossnook-remote-pull-test` artifact has SHA-256
`b7c2a030106128a0c05a0e3e17db555fdc48a56481e9ee8179585901ed78ada4` and is a
static ARMv5TE EABI5 soft-float, non-PIE executable. The published normal Sync
Controller and Reader Sync regression gates also passed:

```text
SYNC CONTROLLER HOST VALIDATION OK
READER SYNC HOST VALIDATION OK
```

## Physical validation

Physical validation passed on Nook Simple Touch using only synthetic
credentials and controlled DNS, SNTP, and HTTPS/KOSync mocks. Transient
runtime files were `/tmp/crossnook-remote-pull-test`,
`/tmp/crossnook-reader-sync-font.ttf`, `/tmp/crossnook-reader-sync.epub`, and
`/tmp/crossnook-reader-sync-testca.crt`. No `/data` path was used and no
intentional internal eMMC persistence was performed.

The initial unmounted gate failed closed before persistence or network and
returned direct remote-shell status `REMOTE_RC=1`:

```text
REMOTE PULL GATE storage=unverified persistence=not-attempted network=not-attempted
```

The external whole-device microSD `/dev/block/mmcblk1` was mounted read/write
as vfat at `/tmp/crossnook-card`. Its sysfs identity was `179:16`, and the
published verifier accepted `/tmp/crossnook-card/crossnook`:

```text
STORAGE VERIFY result=ok errno=0 root=/tmp/crossnook-card/crossnook mount=/tmp/crossnook-card device=179:16
```

An outside-root sentinel at `/tmp/crossnook-remote-pull-sentinel`, containing
`remote-pull-sentinel`, remained unchanged throughout all scenarios.

### Remote present

Real Reader-captured local A was created first:

```text
REMOTE PULL GATE seed-local=ok
```

Remote B was then created in a **separate diagnostic fixture-setup phase**:

```text
REMOTE PULL GATE seed-remote=ok setup-put=1
GET=0
PUT=1
```

That synthetic setup PUT is not part of explicit pull. The measured pull used
one new GET and no PUT, persisted B over A, confirmed local mutation, reported
no remote mutation, and left the open Reader unchanged:

```text
REMOTE PULL GATE outcome=persisted stage=executed get-attempted=1 local-save-attempted=1 local-save=ok local-mutation=confirmed remote-mutation=none local-unchanged=no remote-matched=yes reader-unchanged=yes
TOTAL GET=1
TOTAL PUT=1
```

Per-pull delta:

```text
GET=1
PUT=0
```

The separately invoked published Reader application path then reloaded and
applied persisted B, recaptured the same fixture, required redraw, rendered
successfully, and performed no network operation:

```text
REMOTE PULL GATE apply=import-applied reload=ok redraw-needed=1 recapture-matched=yes render=ok network=not-attempted
TOTAL GET=1
TOTAL PUT=1
```

Per-apply delta:

```text
GET=0
PUT=0
```

### Remote missing

A fresh HTTPS/KOSync mock had no seeded remote state. After creating local A,
the new GET received the protocol's HTTP 200 `{}` missing response. No save or
PUT was attempted, and local A and Reader state remained unchanged:

```text
REMOTE PULL GATE seed-local=ok
REMOTE PULL GATE outcome=remote-missing stage=executed get-attempted=1 local-save-attempted=0 local-save=invalid local-mutation=none remote-mutation=none local-unchanged=yes remote-matched=no reader-unchanged=yes
GET=1
PUT=0
```

Here `local-save=invalid` is only the unused/default bounded field value. The
authoritative evidence is `local-save-attempted=0`; it is not a save failure.
HTTP 404 remains an error and does not mean remote missing.

After unmounting the external storage, the same negative gate again returned
`REMOTE_RC=1` before persistence or network, and the outside-root sentinel was
still `remote-pull-sentinel`:

```text
REMOTE PULL GATE storage=unverified persistence=not-attempted network=not-attempted
```

## Deferred scope and limits

No UI for the explicitly authorized “Use remote progress” operation is
included yet. “Use local”/force upload, automatic retries, stable persisted
device identity, background sync, mount/network lifecycle ownership, conflict
history, and server CAS/version protection remain deferred. The server may
change after GET. Reader apply failure may leave disk at B while Reader remains
at A. ProgressStore cannot report DURABILITY_UNCERTAIN, and FAT32 power-loss
durability is not claimed. FAT32 mode bits do not provide confidentiality for
separately stored credentials.
