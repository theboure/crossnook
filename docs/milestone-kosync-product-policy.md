# KOSync Product Sync Policy Core

**Status: HOST VALIDATION PASS; HARDWARE VALIDATION PASS.**

This milestone adds a synchronous, allocation-free product policy above the
published one-shot KOSync integration. It classifies a completed
`cn_kosync_sync_result`; it does not perform DNS, TLS, HTTP, persistence,
Reader mutation, scheduling, or retries.

## Public API

`src/sync/kosync_policy.[ch]` exports:

```c
int cn_kosync_policy_classify(
    int enabled,
    const cn_kosync_sync_result *sync,
    cn_kosync_product_result *result);
```

The product result contains only an outcome, retry policy, local mutation
state, and remote mutation state. It owns no memory and retains no pointer from
the integration result. No initialization or clear operation is required.

Disabled policy accepts `sync == NULL` and returns `DISABLED`. Enabled policy
requires a completed integration result. Invalid input and unknown enum values
fail closed to `INTERNAL_FAILURE` and `EXPLICIT_ACTION` where output is
available.

## Outcomes

Successful integration decisions map as follows:

| Integration decision | Required evidence | Product outcome |
|---|---|---|
| `NO_CHANGE` | both records present, no writes | `UNCHANGED` |
| `LOCAL_SELECTED` | PUT attempted and confirmed | `UPLOADED` |
| `REMOTE_SELECTED` | save attempted and confirmed | `IMPORTED` |
| `NO_STATE` | both records missing, no writes | `NO_STATE` |
| `AMBIGUOUS` | both records present, no writes | `CONFLICT` |

An inconsistent successful result maps to `INTERNAL_FAILURE`; status `OK`
alone never means synchronized.

Failures preserve authentication, trusted-time, connectivity, TLS validation,
server/protocol, local, configuration, and internal distinctions. Certificate
trust, hostname, and certificate-validation failures map to
`SECURITY_FAILURE`. Generic TLS handshake/protocol failures map to
`SERVICE_FAILURE`. `CN_NETSIMPLE_TLS_INTERNAL` is an implementation failure,
not a security-validation result, and maps to `INTERNAL_FAILURE`.

## Retry Semantics

`NONE` means no retry is needed. `AUTOMATIC_LATER` means only that a future
controller may start a complete new `cn_kosync_sync_once()` transaction after
bounded backoff. `EXPLICIT_ACTION` prohibits automatic retry.

Automatic-later classification is limited to transient time socket I/O,
transient DNS socket/network/timeout/server failures, transient connection and
send/receive failures, HTTP 408, HTTP 429, and HTTP 5xx. Authentication,
configuration, protocol validation, certificate validation, conflict, and
local failures require explicit action.

This milestone contains no retry loop, scheduler, direct PUT retry, or trigger
controller.

## Mutation Evidence

`cn_kosync_sync_result` now records:

```c
int local_save_attempted;
int remote_put_attempted;
```

Both initialize to zero with the containing result. `local_save_attempted` is
set immediately before `cn_progress_store_save()`. `remote_put_attempted` is
set immediately before `cn_kosync_put_progress()`, after local upload data has
been validated and constructed. Selecting a decision does not set either flag.

The existing `local_saved` and `remote_uploaded` fields remain confirmation of
success:

| Attempt | Confirmation | Product mutation |
|---|---|---|
| no | no | `NONE` |
| yes | yes | `CONFIRMED` |
| local save: yes | no | `POSSIBLE` |
| remote PUT: yes | no | detail-dependent |

A failed local save is conservatively `POSSIBLE`. For a failed PUT, connection,
TLS setup/handshake, trust, hostname, certificate, and authentication failures
prove `NONE`. Send failures, response timeouts, malformed/truncated responses,
and stage-ambiguous TLS protocol failures are `POSSIBLE`. The transport layer
does not expose a byte count, so an ambiguous send failure cannot be narrowed
without inventing certainty.

## Ownership

The classifier reads the integration result only during the call. It does not
copy or retain the XPointer, document path, credentials, DNS data, TLS config,
Reader position, or `remote_progress.logical_position`. The caller remains
responsible for clearing `cn_kosync_sync_result` after diagnostics and product
handling.

A future Reader controller must reload an imported position from
`ProgressStore`; it must not borrow the remote progress pointer from this
policy result.

## Future Triggers

Manual, document-open, and document-close triggers will use the same one-shot
operation and classifier. Disabled state must short-circuit before invoking
the integration operation. Trigger handling, settings, credentials, UI,
Reader integration, and background work are outside this milestone.

## Host Gate

Run:

```bash
bash testapp/build-kosync.sh
```

The gate cross-compiles the static ARM EABI5 soft-float non-PIE diagnostic,
runs the synthetic policy matrix under QEMU, and composes product policy with
the focused KOSync integration cases. Coverage includes every top-level sync
status, all DNS/time/transport result classes, HTTP retry boundaries, invalid
enums, name bounds, repeated classification, successful upload/import,
conflict, no state, unchanged state, auth, NXDOMAIN, trusted time, TLS hostname,
malformed protocol, GET timeout, and a PUT that is stored before its response
times out.

The uncertain-PUT fixture proves `remote_put_attempted=1`,
`remote_uploaded=0`, and product mutation `POSSIBLE`; a subsequent GET proves
that the mock stored the record and that no blind PUT retry occurred.

## Hardware Gate

The physical Nook policy smoke matrix passed:

```text
KOSYNC POLICY SMOKE failures=0 -> OK
```

This included physical ARM coverage for disabled policy, all successful product
outcomes, conflict, inconsistent success fail-closed behavior, invalid
configuration, identity failure, possible local mutation after failed save,
local unsupported before PUT, GET authentication rejection, PUT authentication
rejection with proven no mutation, trusted-time result classes, DNS result
classes, transport result classes, confirmed and possible remote mutation,
retryable and non-retryable HTTP statuses, bad JSON/protocol, invalid and
out-of-range enum handling, name helper bounds, and repeated stateless
classification.

The controlled real integration gate first observed a real DNS timeout:

```text
status=dns-failed
decision=none
local=1
remote=0
local-save-attempted=0
remote-put-attempted=0
outcome=connectivity-failure
retry=automatic-later
local-mutation=none
remote-mutation=none
```

This correctly demonstrated transient connectivity classification without
mutation.

The following controlled retry then passed:

```text
status=ok
decision=no-change
local=1
remote=1
saved=0
uploaded=0
identity=ok
load=ok
time=ok
dns=ok
kosync=ok
http=200
transport=ok
remote_progress=3210
local-save-attempted=0
remote-put-attempted=0
outcome=unchanged
retry=none
local-mutation=none
remote-mutation=none
```

Transcript verification returned:

```text
NO PUT OBSERVED -> PASS
```

All physical state and credentials were kept under `/tmp`. No intentional
internal eMMC write was performed.

The ARM diagnostic provides `--policy-smoke`, and `--sync-once` prints product
outcome, retry policy, and both mutation states.

The controlled no-change proof must report:

```text
outcome=unchanged retry=none local-mutation=none remote-mutation=none
```

The server transcript confirmed that this no-change invocation issued no PUT.

## Known Limitation

GET-missing followed by unconditional PUT retains the published
concurrent-create race. KOSync exposes no conditional PUT, revision, or ETag.
An automatic-later policy value does not solve this race and does not authorize
a direct PUT retry; a future controller must restart the full GET/reconcile
transaction.
