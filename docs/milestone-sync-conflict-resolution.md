# Sync Conflict Resolution UI Integration

**Status: IMPLEMENTATION COMPLETE; HOST VALIDATION PASS; HARDWARE VALIDATION PENDING.**

Normal manual Reader Sync still captures and persists A before normal sync.
Only its exact `CONTROLLER_COMPLETE` / pre-save OK / integration stage /
sync status OK / `AMBIGUOUS` / product `CONFLICT` result can open the three
choice Reader overlay. Other normal outcomes never authorize a resolution.
The existing `cn_reader_sync_result` retains all typed controller evidence.
Published automatic normal-sync outcomes retain their existing behavior; the
device diagnostic does not enter conflict choices on those outcomes.

The application keeps an owned copy of Reader A and of the exact selected EPUB
path for the modal lifetime. Before either destructive choice, the new
`cn_reader_sync_resolve_conflict()` verifies the original result, Reader state,
book path, current Reader position, and currently persisted local position.
A mismatch returns STALE without GET, PUT, save or Reader movement. This is a
single-writer application guard, not an atomic filesystem/server CAS. Local
choice reuses the A persisted by the initial manual sync; it does **not** save
Reader a second time. It calls only `cn_sync_push_local_current_book()` (direct
PUT; no discovery GET). A lost PUT response can yield remote mutation POSSIBLE:
the status must never say definitely failed or automatically retry.

Remote choice calls `cn_sync_pull_remote_current_book()` for a **fresh GET**;
if it persisted remote B/C, only then calls `cn_reader_sync_apply_persisted()`.
Only a successful Reader apply requests redraw. If remote is now missing or
GET fails, disk and Reader remain A and no apply runs. If disk becomes B but
Reader apply fails, the result reports mismatch; the dedicated application
loop must not close-save old Reader A over B. Neither choice infers freshness,
reuses an original conflict payload or changes normal sync semantics.

`cn_ui` keeps underlying `CN_UI_READER` while a small conflict overlay is
shown. Three rows are ordered Use this device / Use remote progress / Cancel;
Cancel starts highlighted. PAGE_NEXT/PREV select without moving the book;
TOUCH_UP highlights only; MENU confirms once; BACK/HOME dismiss to Reader
without closing or saving. Long-power retains its existing behavior. After a
choice, a bounded acknowledgement overlay blocks repeated MENU and can be
dismissed by BACK/HOME. Cancel does no resolution save, GET or PUT; the initial
manual save-first A has already occurred.

The verified-card diagnostic validates external whole-device microSD
`/dev/block/mmcblk1` (`179:16`) at `/tmp/crossnook-card`, rooted at
`/tmp/crossnook-card/crossnook`, **before opening persistent stores**. It
rechecks the mount before later explicit choices. No `/data` or intentional
internal eMMC persistence. Only synthetic credentials and a caller-supplied
synthetic runtime device ID are used. A real-account bootstrap/runtime ID is
outside this milestone; no ID is generated or persisted. FAT32 has no useful
Unix permission confidentiality and ProgressStore OK is not a power-loss
durability guarantee.

`bash testapp/build-sync-conflict-ui.sh` builds a static ARMv5TE EABI5
soft-float non-PIE diagnostic. Host QEMU uses real Reader/UI/EPUB and
ProgressStore with fake controller boundaries; object-symbol checks require
the published explicit controllers and Reader apply, rejecting raw KOSync
GET/PUT/serializer and normal-sync references in the UI/helper. Published
Sync Controller, Manual Reader Sync, Explicit Remote Pull and Explicit Use
Local host regressions must also pass.

```text
SYNC CONFLICT UI SMOKE failures=0 -> OK
SYNC CONFLICT UI HOST VALIDATION OK
SYNC CONTROLLER HOST VALIDATION OK
READER SYNC HOST VALIDATION OK
REMOTE PULL HOST VALIDATION OK
LOCAL PUSH HOST VALIDATION OK
```

The focused artifact `testapp/crossnook-sync-conflict-ui-test` is static
ARMv5TE EABI5 soft-float, non-PIE; SHA-256
`df69d7d99436b4726353daaabef49f35aa15db464f6d4b1bcdc19e2744931970`.

## Later Nook physical gate (not executed in this pass)

The CLI accepts no credential arguments:

```text
/tmp/crossnook-sync-conflict-ui-test --physical <seed-remote|run|verify-remote> conflict <mount> <verified-root> 179 16 <font> <epub> <dns-ip> <dns-port> <sntp-ip> <sntp-port> <test-ca>
```

Use the already isolated synthetic `controller-conflict` fixture on the
verified card. First with card unmounted, `run` must report storage unverified
and persistence/network not attempted; check the outside-root sentinel.
Mount and positively verify the external card. Start controlled DNS/SNTP/
HTTPS/KOSync mocks with the synthetic account. `seed-remote` separately PUTs
Reader-generated B; do not include this setup PUT in measured action deltas.

For each run, the app opens Reader A, waits for physical MENU, then invokes
the published manual save-first sync. Count the normal sync GET separately.
Require an exact CONFLICT and rendered three-choice overlay with Cancel
highlighted. NEXT/PREV must change selection, not the Reader position.
BACK cancels: resolution GET=0/PUT=0, disk/Reader A unchanged. Repeat setup
and choose local with PREV/PREV/MENU: resolution GET=0/PUT=1 and Reader/disk A
unchanged; use **separate** `verify-remote` GET to confirm remote A. Reseed
remote B in a separate setup process, repeat conflict, choose remote with
PREV/MENU: resolution GET=1/PUT=0, disk becomes fresh B, Reader applies B,
and the app renders/flushes; require `reader-matched-persisted=yes render=ok`.
The dedicated one-shot device loop does **no implicit Reader exit-save**, even
after a failed apply, so it cannot overwrite persisted B with Reader A.
Dismiss the bounded result acknowledgement with BACK or HOME before the run
process exits; repeated MENU cannot reissue the operation. Check bounded
status, framebuffer, and sentinel throughout. Stop processes, unmount, and
repeat refusal with no persistence
or network. Never include setup PUT, normal-sync GET or verification GET in
the explicit resolution-action delta.

No generic widget/notification framework, newest-wins policy, timestamps,
CAS/revisions, retries, background sync, account/settings UI, persistent
device-ID redesign, or multi-book conflict center is included. The server
can change after a GET/PUT, and a separate local writer can race the snapshot
guard; there is no atomic compare-and-swap.
