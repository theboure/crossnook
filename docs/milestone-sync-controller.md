# Sync Controller Core

**Status: IMPLEMENTATION COMPLETE; HOST VALIDATION PASS; HARDWARE VALIDATION PASS.**

## Boundary and one-shot API

`src/sync/sync_controller.[ch]` adds one explicit synchronous operation:

```c
cn_sync_controller_result cn_sync_current_book(
    const cn_sync_controller_config *config);
```

`config` supplies initialized Settings and Credential stores, an already-open
ProgressStore, the exact document path supplied to Reader, an explicit runtime
`device_id`, and DNS/TLS/time dependencies. The caller owns those objects and
keeps them alive for the call. Startup must verify portable storage, prepare
Storage Layout, then supply stores opened under that verified root. The
controller neither discovers mounts nor chooses a storage root or Wi-Fi state.

The controller loads Settings. `MISSING` gives published disabled defaults and
passes `enabled=0, sync=NULL` to `cn_kosync_policy_classify()`. Explicitly
disabled Settings do the same: no Credentials load, identity, time, DNS, network,
or integration call. Corrupt/unsupported/failed Settings instead return a
distinct `SETTINGS_FAILED` stage and configuration-failure outcome. When enabled,
it loads Credentials. `MISSING` maps to preflight `AUTH_REQUIRED`; corrupt or
unsupported credentials map to configuration failure, while other credential
filesystem failures map to local failure. The exact Credential Store result is
retained. These **preflight-only** mappings are necessary because product
policy accepts a completed integration result, not a store-load result.

After credential load, validate enabled Settings, explicit nonempty bounded
UTF-8/control-free `device_id`, and DNS/TLS/trusted-time inputs. A missing or
invalid ID is configuration failure before any network call; it is not generated
or stored. Initialize the existing HTTPS KOSync client with Settings base URL
and loaded username/userkey. Call `cn_kosync_sync_once()` exactly once, then
`cn_kosync_policy_classify(1, &sync, &product)` exactly once. The controller
never performs an independent GET, PUT, progress save, conflict choice, or
retry. It copies bounded status and evidence only, clears the owned integration
result and volatile-clears credential/client copies before returning.

`cn_sync_controller_result` owns no pointers or secret text. It carries a
controller stage, one published `cn_kosync_product_result` (outcome, retry,
local and remote mutation), store results, integration status/decision and
underlying bounded result codes, plus local-save and remote-PUT attempted/
confirmed flags. If product classification fails, the result uses explicit
`CLASSIFICATION_FAILED` and `INTERNAL_FAILURE`/`EXPLICIT_ACTION`, retains
attempt/confirmation flags, and conservatively marks attempted mutation
`POSSIBLE` (or `CONFIRMED` when success and attempt are consistent). This
fallback is flagged as conservative; it does not erase possible mutation.

## Ownership, partial success, retry

The existing integration derives separate path-keyed local identity and
KOReader Binary server identity, loads local ProgressStore state, establishes
trusted time when requested, resolves DNS, performs HTTPS GET, and makes at
most one permitted local save or remote PUT. It compares canonical XPointers:
same position means no change even if percentage differs; different positions
mean conflict with no mutation. This controller does not reimplement those
rules. It synchronizes the **already persisted** position, not live Reader
state. A later Reader/manual-trigger layer must capture a desired current
position first, and later decide whether to apply an imported position.

Product policy alone classifies `NONE`, `CONFIRMED`, or `POSSIBLE` local and
remote mutation and `NONE`, `AUTOMATIC_LATER`, or `EXPLICIT_ACTION` retry. The
controller returns those values without a retry loop. Any future automatic
retry starts a **new full GET/reconciliation**, never a blind PUT replay after
an uncertain outcome. GET/auth/time/DNS/transport results remain distinguishable
through integration result codes. Integration's remote-only path performs a
local save after GET; its local-only path performs PUT but **no subsequent
local save**. There is no PUT-success-then-local-save-failure outcome in v1.

`cn_progress_store_save()` returns only its published `cn_progress_result` and
does not report a directory-fsync failure as `DURABILITY_UNCERTAIN`. Therefore
controller `local_saved` means only ProgressStore reported `OK`; it cannot
assert FAT32 power-loss durability or invent a local durability-uncertain
status. Failed local save remains conservatively `POSSIBLE` per policy.
Deployment must keep the verified mount stable across this operation.

TIME_ESTABLISH delegates SNTP and clock setting to the integration and requires
explicit `cn_time_config` and the ordinary TLS time source. CALLER_ESTABLISHED
is accepted only as an explicit caller-selected guarantee of valid certificate
time; the controller does not bootstrap time itself. No Wi-Fi readiness API is
exposed for this operation; deployment supplies networking, and existing DNS/
transport status flows through integration and policy.

## Focused host gate

`bash testapp/build-sync-controller.sh` cross-compiles a static ARMv5TE EABI5
soft-float non-PIE diagnostic and runs under QEMU. Host `--smoke` uses
linker-wrapped Settings/Credential loads and integration, with the **real**
Settings validation, KOSync client initialization, and product policy. The
wrappers record call order/count and inject completed integration results; no
real DNS, SNTP, TLS, HTTP, or physical device access occurs in host smoke.
Coverage includes disabled/missing/invalid preflight, synthetic credential
clearing, one integration and policy call at most, all successful decisions,
auth/time/connectivity failures, definite and uncertain PUT outcomes, failed
local save, conservative classification failure, no mutation on no-change or
conflict, no post-PUT local save, redacted captured stdout/stderr, and rejected
CLI credential arguments. Existing integration tests—not these injected tests—
prove the actual canonical-XPointer comparison, GET/PUT sequencing, and
transport behavior. The ProgressStore API's absence of durability-uncertain
and the absence of PUT-then-local-save are explicit non-representable cases.

```text
SYNC CONTROLLER SMOKE failures=0 -> OK
SYNC CONTROLLER HOST VALIDATION OK
```

## Physical hardware validation

The static ARMv5TE EABI5 soft-float non-PIE diagnostic was deployed only under
`/tmp` on the physical Nook. Local artifact SHA-256 matched
`50f89ec6f3fa7989e49b6bdf45c0ca17eeeb90e987f6ea5021ee969c79d2babf`.
Only internal synthetic credentials and a synthetic device ID were used; no
real KOSync account or credential value appeared in the status-only diagnostic
output. Do not reproduce raw mock request headers, credential-bearing server
transcripts, or username fields.

With the external card initially unmounted, the diagnostic refused storage
verification **before** attempting persistence or network activity:

```text
SYNC CONTROLLER GATE storage=unverified persistence=not-attempted
REMOTE_RC=1
```

`REMOTE_RC=1` was measured in the remote Nook shell, not inferred from the host
PowerShell `adb` invocation's `$LASTEXITCODE`. The already-published verifier
then accepted the pre-existing application root on the whole-device external
microSD (`/dev/block/mmcblk1`, expected `179:16`):

```text
STORAGE VERIFY result=ok errno=0 root=/tmp/crossnook-card/crossnook mount=/tmp/crossnook-card device=179:16
```

The diagnostic created isolated `controller-<scenario>` fixtures below that
verified root. `prepare` refused pre-existing scenario directories or Settings/
Credential files. `seed` created only synthetic local progress for the selected
scenario. Controlled DNS/SNTP/HTTPS mock infrastructure supplied network and
time dependencies. The observed status-only results were:

```text
# DISABLED
SYNC CONTROLLER GATE prepare=ok
SYNC CONTROLLER GATE stage=disabled outcome=disabled retry=none local-mutation=none remote-mutation=none local-save-attempted=0 remote-put-attempted=0 credential-loads=0 sync-calls=0 policy-calls=1

# NO_STATE
SYNC CONTROLLER GATE prepare=ok
SYNC CONTROLLER GATE stage=integration outcome=no-state retry=none local-mutation=none remote-mutation=none local-save-attempted=0 remote-put-attempted=0 credential-loads=1 sync-calls=1 policy-calls=1

# NO_CHANGE
SYNC CONTROLLER GATE prepare=ok
SYNC CONTROLLER GATE seed=ok
SYNC CONTROLLER GATE stage=integration outcome=unchanged retry=none local-mutation=none remote-mutation=none local-save-attempted=0 remote-put-attempted=0 credential-loads=1 sync-calls=1 policy-calls=1
GET_COUNT=1
PUT_COUNT=0

# UPLOAD
SYNC CONTROLLER GATE prepare=ok
SYNC CONTROLLER GATE seed=ok
SYNC CONTROLLER GATE stage=integration outcome=uploaded retry=none local-mutation=none remote-mutation=confirmed local-save-attempted=0 remote-put-attempted=1 credential-loads=1 sync-calls=1 policy-calls=1
GET_COUNT=1
PUT_COUNT=1

# CONFLICT
SYNC CONTROLLER GATE prepare=ok
SYNC CONTROLLER GATE seed=ok
SYNC CONTROLLER GATE stage=integration outcome=conflict retry=explicit-action local-mutation=none remote-mutation=none local-save-attempted=0 remote-put-attempted=0 credential-loads=1 sync-calls=1 policy-calls=1
GET_COUNT=1
PUT_COUNT=0
```

Disabled invoked policy once but loaded no credentials and made no integration
call. The no-change fixture had the **same canonical XPointer and a different
percentage**, which did not trigger PUT or establish a freshness ordering.
Local-only upload performed exactly one PUT and reported confirmed remote
mutation. Different XPointers remained conflict/ambiguous with no mutation;
the controller invented no newer-record selection rule.

The outside-root `/tmp/crossnook-controller-sentinel` retained its original
`controller-sentinel` contents throughout. After unmount, no
`crossnook-card` mount remained and the diagnostic again returned:

```text
SYNC CONTROLLER GATE storage=unverified persistence=not-attempted
REMOTE_RC=1
```

No `/data` or internal eMMC persistence path was used. This gate validates
the observed orchestration and status/mutation evidence, not power-loss
durability on FAT32: ProgressStore cannot expose `DURABILITY_UNCERTAIN`.

Deferred: stable persisted ID, live Reader capture/restore, manual UI trigger,
scheduling/background sync, automatic retries, mount lifecycle management,
GET-missing/PUT concurrent creation, and better ProgressStore durability
reporting.
