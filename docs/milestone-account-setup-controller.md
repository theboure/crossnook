# Account Setup Controller

This milestone adds the application-facing orchestration layer for first-time
KOSync setup. It composes the published local bootstrap, persistent device
identity, explicit activation, and persisted-profile APIs. It does not change
their storage or network semantics.

## Boundary

The caller supplies a positively verified, initialized, and prepared external
microSD `cn_storage_layout`, plus the DNS, trusted-time, and verified-TLS
runtime configuration. The controller does not discover mounts, create roots,
use `/data`, prepare storage, manage Wi-Fi, or render UI.

The public operations are:

```c
cn_account_setup_submit(layout, input, intent, runtime);
cn_account_setup_activate_existing(layout, runtime);
```

`NEW_OR_RESUME` is limited to no-account state and a compatible settings-only
partial bootstrap. It never rewrites a complete disabled account. A complete
disabled account is retried with `activate_existing`.

`REPLACE_DISABLED` is an explicit correction operation. It is allowed only for
a complete valid disabled account and may change username, password/userkey,
base URL, and device name by reusing `cn_account_bootstrap()`. It is never
accepted for an enabled account. No raw password, username, userkey, URL, or
derived credential value appears in a report.

## State Machine

The controller first observes settings and credentials without deriving the
submitted password:

| Local state | Result/action |
|---|---|
| Missing settings and credentials | `NEW_OR_RESUME` bootstraps disabled state. |
| Valid disabled settings, missing credentials | `NEW_OR_RESUME` resumes only when URL and device name match. |
| Missing settings, valid credentials | `INCOMPLETE_STATE`; explicit recovery is required. |
| Valid disabled settings and credentials | `NEW_OR_RESUME` is refused; `activate_existing` is the retry path. |
| Enabled settings | `ALREADY_ENABLED` before credential/identity loading, password derivation, network, writes, or profile verification. |
| Corrupt/unsupported settings or credentials | `CORRUPT_OR_UNSUPPORTED_STATE`; no repair or overwrite. |

After bootstrap, identity is loaded read-only. Only a genuinely missing identity
may use `cn_device_identity_load_or_create()`. Malformed, unsupported, or
checksum-invalid identity state is never rotated.

Activation is delegated to `cn_sync_activate()`. Its permitted network work is
the configured trusted-time operation, DNS, verified HTTPS, and the single
`GET <base-path>/users/auth`. Progress GET and PUT are not performed.

After a fresh successful activation, the controller performs a read-only
`cn_persisted_sync_profile_load()` and requires `READY`. A failed check reports
`ENABLED_PROFILE_UNAVAILABLE`; enabled settings are not rolled back. The check
is not performed for `ALREADY_ENABLED`.

## Durability and Retry

There is no cross-file transaction. Bootstrap settings, credentials, identity,
and activation enablement are separate pathname operations. Any ordinary
failure stops the call. Any `DURABILITY_UNCERTAIN` result also stops the call;
the controller does not infer old or new durable state and does not retry
activation in the same call.

After process death, the next invocation re-observes state. A visible enabled
settings file returns `ALREADY_ENABLED`; a visible disabled file can retry
activation; corrupt or unreadable state fails closed. Wrong credentials leave
the disabled credentials persisted and require explicit `REPLACE_DISABLED` for
a changed password. DNS, time, TLS, and service failures can retry through
`activate_existing` without rewriting account material. A normal final settings
save failure requires remote authentication again on the next attempt.

## Mutation Boundary

- Bootstrap may save only `config/settings.conf` with `enabled=false` and
  `state/credentials`.
- Identity readiness may create only `config/device-id`, and only when absent.
- Activation may save only the final `settings.conf` enable transition.
- Profile verification is read-only.
- Credentials, identity, progress, outside-root sentinels, and unrelated files
  are not changed by activation.

The controller owns no root or mount selection. The store APIs retain their
existing atomic-save and durability-uncertain contracts; this milestone adds no
rollback marker or transaction claim.

## Validation

The focused gate is:

```text
bash testapp/build-account-setup-controller.sh
```

It builds a static ARMv5TE EABI5 soft-float non-PIE diagnostic and runs a
deterministic QEMU matrix covering fresh success, rejected auth and explicit
correction, disabled retries, partial/orphan state, enabled fast path, URL and
device correction, malformed state, bootstrap/identity/activation failures and
uncertainty, profile-unavailable success, and zero progress operations. Output
is scanned for synthetic account material.

The proposed guarded physical mode is:

```text
/tmp/crossnook-account-setup-controller-test --physical \
  <mountpoint> <root> <major> <minor> <dns-ip> <dns-port> \
  <sntp-ip> <sntp-port> <base-url> <ca-path> <accepted|rejected> \
  <outside-root-sentinel>
```

It verifies the supplied external card before preparing or writing the
disposable root. With controlled DNS/SNTP/HTTPS/KOSync fixtures it exercises
rejected authentication, explicit correction, accepted activation, and the
already-enabled fast path while reporting no progress operations. Separate
clean roots should be used for independent physical cases. Physical validation
has not been performed by this milestone.

## Deferred

Visual account-entry UI, deactivation, deletion, enabled-account switching or
credential rotation, normal/background scheduling, Reader/progress behavior,
Wi-Fi lifecycle changes, storage/root ownership, and automatic repair of
corrupt state remain deferred.
