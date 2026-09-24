# Manual Reader Sync Integration

**Status: IMPLEMENTATION COMPLETE; HOST VALIDATION PASS; HARDWARE VALIDATION PASS.**

## Application boundary

`src/app/reader_sync.[ch]` composes the published UI-owned Reader,
path-keyed ProgressStore, and one-shot Sync Controller. This is an explicit
application operation, not a new KOSync algorithm or a UI trigger:

```c
cn_reader_sync_result cn_reader_sync_manual_once(
    const cn_reader_sync_config *config);
```

The caller supplies `cn_ui *`, an already-open `cn_progress_store *`, and an
application-owned `cn_sync_controller_config` template. Require the UI to be in
the real EPUB Reader state with a selected EPUB and an open Reader. The selected
book's **exact lexical path** is used for both `cn_book_identity_from_path()`
and the controller's `document_path`. The supplied store must be the same one
used by the controller; conflicting non-NULL template path/store fields are
rejected before capture or save. Startup/deployment owns verified external
storage, Settings/Credentials, time, DNS, TLS, device ID, mount and network
lifecycle. Neither module makes up a device ID or discovers storage.

The existing device diagnostic `src/app/reader-test.c` already demonstrates
capture/save before BACK/HOME/exit and restore after EPUB open. Those static
diagnostic helpers remain unchanged. This reusable application seam uses the
same existing UI forwarding and ProgressStore APIs; Reader, UI, ProgressStore,
sync protocol/integration/policy/controller, and networking are unchanged.

## Exact save-first sequence

1. Verify Reader/book/configuration, derive path-keyed local book identity.
2. Initialize a `cn_progress_record`, capture the canonical opaque UTF-8
   `cn_reader_position` (up to 65,536 bytes, with `progress_10000` in
   `0..10000` or `-1`) through `cn_ui_reader_get_position()` while the book
   remains open, and call `cn_progress_store_save()` **once**.
3. On identity, capture, or save failure: clear owned position and return with
   **no controller call**. On success, clear capture, set exact path/store in a
   local copy of the template, call `cn_sync_current_book()` **once**, and
   return its entire bounded result. No GET, PUT, additional save, retry,
   conflict resolution, rendering, or framebuffer flushing occurs here.
4. Only if the integration-stage controller result is `IMPORTED`, initialize
   a **new** ProgressStore record, reload it by the same identity, then use
   `cn_ui_reader_goto_position()` on that **owned persisted position**. Never
   borrow the KOSync remote position and never fall back to percentage. A
   successful goto reports `redraw_needed=1`; the application caller performs
   `cn_ui_render()` and its usual display flush separately.

`cn_reader_sync_result` has no owned pointers, paths, XPointers, credentials,
or raw HTTP data. It preserves the full controller result and separately
reports the application phase, `pre_save_result`, `import_reload_result`,
pre-save/controller invocation flags, redraw need, possible Reader movement,
and possible disk/Reader mismatch. An application pre-save can have written
progress even when the controller reports `local_mutation=NONE` (including
CONFLICT or DISABLED). These are different mutation scopes.

### Crucial reachable-outcome boundary

Every successful manual pre-save makes local progress present. With remote
missing the published integration can upload; with the **same canonical
XPointer** it returns UNCHANGED even when percentages differ; with a
**different XPointer** it returns CONFLICT, with no PUT. A normal successful
save-first call cannot produce remote-only IMPORTED or NO_STATE. This seam
neither deletes/skips local progress nor chooses a fresher side to force an
import. The defensive `IMPORTED` branch remains forward-compatible and is
host-tested using an injected controller that first persists B; it is **not**
represented as a normal reachable save-first transaction.

For a separately authorized, already-persisted B, the narrow helper
`cn_reader_sync_apply_persisted(ui, store)` performs only identity derivation,
reload, and Reader goto. It performs **no network, pre-save, mode selection, or
sync operation** and is used to validate the application mechanics of applying
persisted state. It is not a hidden pull mode.

If reload fails after a reported import, disk state is uncertain and Reader
need not match it. If goto fails, Reader's validated position does not move,
but disk may contain B while Reader remains at A. Neither case rolls back or
silently overwrites B. An existing BACK/HOME/exit save in `reader-test.c`
could overwrite B with A if a future UI ignores the mismatch result. Successful
goto moves the in-memory Reader but does not render or persist. Exact
byte-for-byte recapture after goto is not guaranteed for arbitrary mid-page
XPointers; the focused fixture may test equality without claiming universal
equality. A caller must honor `redraw_needed` and separately report a display
failure if render/flush fails after an applied move.

ProgressStore's `OK` does **not** distinguish directory-fsync failure or
guarantee FAT32 power-loss persistence; no `DURABILITY_UNCERTAIN` is exposed.
FAT32 also offers no meaningful Unix-mode confidentiality for credentials.
There is no scheduling, retry loop, UI control, stable persisted device ID,
or mount/network lifecycle policy in this milestone.

## Focused host validation

`bash testapp/build-reader-sync.sh` builds a static ARMv5TE EABI5 soft-float
non-PIE diagnostic. QEMU uses a real Reader/UI/EPUB and real temporary
ProgressStore, linker-wrapping only the Sync Controller and targeted failure
injection points. It performs **no real network**. It covers closed/no-book
state, template mismatch, identity/capture/pre-save failures, exact saved A,
one controller call, unchanged Reader on upload/no-change/no-state/conflict/
failures, and defensive injected import into a real ProgressStore followed by
reload and real Reader goto. Missing/corrupt/unsupported reloads, an unresolvable
XPointer, repeated operations, and the independent apply-persisted helper are
checked. Test output reports bounded statuses, never a raw XPointer or secret;
published Reader's own page diagnostics are filtered from the displayed host
transcript. No production API callback is added solely for testing.

```text
READER SYNC SMOKE failures=0 -> OK
READER SYNC HOST VALIDATION OK
```

## Physical hardware validation

The static ARMv5TE EABI5 soft-float non-PIE artifact was deployed only to
`/tmp/crossnook-reader-sync-test` on the physical Nook. Its local SHA-256
matched `365cba4da89606c2005cd6b700f5cae1838c506a594abb49e5f9dc4c2c017d46`.
Diagnostic usage executed successfully on the target. The font, EPUB, and
test CA were also deployed only under `/tmp`:

```text
/tmp/crossnook-reader-sync-font.ttf
/tmp/crossnook-reader-sync.epub
/tmp/crossnook-reader-sync-testca.crt
```

Initially, no `crossnook-card` mount was present. The diagnostic rejected
unverified storage before persistence; the exit code was measured **in the
remote shell**, not inferred from PowerShell's `adb` `$LASTEXITCODE`:

```text
READER SYNC GATE storage=unverified persistence=not-attempted
REMOTE_RC=1
```

The already-published verifier then accepted the read/write `vfat` filesystem
on whole-device external microSD `/dev/block/mmcblk1` (`179:16`) at
`/tmp/crossnook-card`:

```text
STORAGE VERIFY result=ok errno=0 root=/tmp/crossnook-card/crossnook mount=/tmp/crossnook-card device=179:16
```

Only diagnostic-internal synthetic credentials and controlled DNS, SNTP, and
HTTPS/KOSync mocks were used. The operator's outside-root sentinel at
`/tmp/crossnook-reader-sync-sentinel` retained its exact
`reader-sync-sentinel` contents throughout. No `/data` path or intentional
internal eMMC persistence was used. Bounded status-only physical results:

```text
# UPLOAD (Reader A captured and saved before one controller call)
READER SYNC GATE phase=controller-complete pre-save=ok controller-invoked=1 outcome=uploaded retry=none local-mutation=none remote-mutation=confirmed redraw-needed=0 movement-possible=0 reader-unchanged=yes persisted-a=yes
GET=1
PUT=1

# NO_CHANGE (same controlled mock server, not restarted after UPLOAD)
READER SYNC GATE phase=controller-complete pre-save=ok controller-invoked=1 outcome=unchanged retry=none local-mutation=none remote-mutation=none redraw-needed=0 movement-possible=0 reader-unchanged=yes persisted-a=yes
# Cumulative server events: GET=2, PUT=1; invocation delta:
GET=1
PUT=0

# CONFLICT (controlled different-position remote fixture)
READER SYNC GATE phase=controller-complete pre-save=ok controller-invoked=1 outcome=conflict retry=explicit-action local-mutation=none remote-mutation=none redraw-needed=0 movement-possible=0 reader-unchanged=yes persisted-a=yes
# Cumulative server events: GET=3, PUT=1; invocation delta:
GET=1
PUT=0

# APPLY-DEMO (no sync/controller request)
READER SYNC GATE phase=import-applied reload=ok controller-invoked=0 redraw-needed=1 movement-possible=1 recapture-matched=yes render=ok
GET=0
PUT=0
```

UPLOAD confirmed that pre-saved A was persisted, exactly one GET and PUT
occurred, and Reader stayed at A. The next invocation used the same remote A
without restarting the mock: `UNCHANGED` made one GET and no PUT. The distinct
remote XPointer returned `CONFLICT` with no PUT or Reader movement. No
percentage freshness or automatic conflict resolution was introduced. The
`local-mutation=none` product field refers only to the controller, not the
application's separate successful pre-save.

APPLY-DEMO persisted authoritative B, reloaded it, applied it to an already-open
Reader, recaptured the target for **this fixture**, and rendered into memory.
No controller, GET, or PUT occurred; actual screen/display flushing remains
the caller's responsibility. This is **not** “save A then remote-only import
B”, which is unreachable from this save-first operation. Recapture equality
for this fixture does not promise exact equality for arbitrary mid-page tokens.

After unmount, no `crossnook-card` mount remained. The diagnostic again
refused persistence; the outside-root sentinel was still unchanged:

```text
READER SYNC GATE storage=unverified persistence=not-attempted
REMOTE_RC=1
```

The physical results do not establish a FAT32 power-loss durability guarantee:
ProgressStore still cannot surface `DURABILITY_UNCERTAIN`. No raw request
headers, credential-bearing transcript content, usernames, or userkeys belong
in this milestone document.

Remaining work: user-authorized pull/discard-local policy if desired, manual
UI trigger, persistence/Reader mismatch handling on exit after failed apply,
stable device identity, optional better ProgressStore durability reporting,
and application-managed mount/network lifecycle.
