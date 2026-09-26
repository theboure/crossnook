# Explicit Sync Activation Core

This milestone adds the one intentional transition from the locally bootstrapped
disabled state to enabled KOSync. Account bootstrap remains local-only and still
writes `kosync_enabled=false`; activation is a separate, explicit operation.

## Policy

Activation is remote-authenticated. Local settings, credentials, and the existing
installation identity must first load and validate successfully. Activation then
establishes the caller-selected trusted-time policy, resolves DNS, configures
verified HTTPS, and performs the pinned KOSync authorization operation:

```text
GET <configured-base-path>/users/auth
```

The request uses the existing vendor `Accept` header and `x-auth-user` plus
`x-auth-key` headers. HTTP 200 accepts the account; HTTP 401 is
`CN_KOSYNC_AUTH_FAILED`; other HTTP responses retain the existing service-error
semantics. No document ID, progress GET, PUT, local progress, or remote progress
is involved. The operation is read-only.

## Disabled Preflight

`cn_persisted_sync_profile_load()` is unchanged. Missing or disabled settings
still return `DISABLED` before credentials and identity are attempted. Activation
uses a dedicated preflight that directly reuses the Settings Store, Credential
Store, and load-only Device Identity Store APIs. It does not call
`cn_device_identity_load_or_create()`, repair files, prepare storage, or replace
corrupt state.

Missing, malformed, unsupported, or checksum-invalid settings, credentials, and
identity state fails closed. Missing settings are not interpreted as activation
defaults. An already enabled settings file returns `ALREADY_ENABLED` before
credentials, identity, runtime networking, or writes are attempted.

## API

```c
typedef struct cn_sync_activation_runtime {
    const cn_dns_config *dns;
    const cn_tls_config *tls;
    cn_kosync_sync_time_policy time_policy;
    const cn_time_config *time;
} cn_sync_activation_runtime;

cn_sync_activation_report cn_sync_activate(
    const cn_storage_layout *prepared_layout,
    const cn_sync_activation_runtime *runtime);
```

The layout must already be externally verified, initialized, and prepared by the
caller. Runtime DNS, CA, TLS, and trusted-time configuration is borrowed for the
call and never persisted. Reports contain typed component results and attempted,
authorization, save, and possible-visible-enable flags, but no account material.

## State Transition

The exact ordering is:

1. Load and validate settings; return `ALREADY_ENABLED` when enabled.
2. Load existing credentials and existing device identity.
3. Validate runtime inputs, establish trusted time when requested, and resolve
   the service hostname.
4. Route verified HTTPS through the numeric DNS answer while retaining the
   original hostname for HTTP Host, TLS SNI, and certificate verification.
5. Perform read-only `GET /users/auth`.
6. Set `kosync_enabled=true` in the already loaded settings and perform exactly
   one final settings save.

No settings save occurs before authorization succeeds. Credentials, identity,
Reader state, and progress are never saved by activation. A failed preflight,
time/DNS/TLS operation, authentication rejection, service failure, or ordinary
settings-save failure leaves the previously persisted disabled file unchanged.

## Durability Uncertainty

The Settings Store can return `CN_SETTINGS_DURABILITY_UNCERTAIN` after the new
file has been renamed but before directory durability is confirmed. Activation
reports `DURABILITY_UNCERTAIN`, retains `remote_auth_succeeded=1` and
`settings_save_attempted=1`, and sets `settings_enable_visible_possible=1`. It
does not claim durable success or claim that the persisted setting remains false.
The next activation reloads settings normally; if the enabled file is visible it
returns `ALREADY_ENABLED` without another network request or write.

## Security Boundary

The userkey is account-equivalent secret material. It is loaded transiently,
copied only into the existing bounded client structure, and cleared with the
project byte-wipe convention on every activation exit. Reports and diagnostics
never contain usernames, userkeys, passwords, credential hashes, or digests.
Activation requires verified HTTPS and the existing trusted-time/DNS policy. The
physical validation mode uses only synthetic credentials, an isolated verified
external-card root, and controlled DNS/SNTP/HTTPS/KOSync mocks.

## Validation

The focused gate is:

```text
bash testapp/build-sync-activation.sh
```

It exercises successful and rejected authorization, runtime failures, malformed
and incomplete local state, idempotent active state, ordinary save failure,
post-rename durability uncertainty, retries, byte stability, and zero progress
GET/PUT calls. The Python mock also exposes deterministic `/users/auth` 200/401
behavior and redacted authorization events.

The guarded physical mode is:

Start the controlled HTTPS/KOSync mock with the same deployment base path that
appears in the client URL. The mock accepts exact root deployment paths by
default; this invocation explicitly exercises `/kosync`:

```text
python testapp/https_mock_server.py --host 0.0.0.0 --port 18443 \
  --cert testapp/pki/server.crt --key testapp/pki/server.key \
  --base-path /kosync --transcript <transcript-path> \
  --sni-log <sni-log-path> --host-log <host-log-path> --quiet
```

The DNS mock must answer the HTTPS hostname with the controlled PC address, and
the SNTP mock must be reachable from the Nook. Then run the diagnostic:

```text
/tmp/crossnook-sync-activation-test --physical \
  /tmp/crossnook-card \
  /tmp/crossnook-card/crossnook-sync-activation-30 \
  179 16 <dns-ip> <dns-port> <sntp-ip> <sntp-port> \
  https://<controlled-host>:<port>/kosync <ca-path> accepted \
  /tmp/crossnook-card/crossnook-sync-activation-30.sentinel
```

The root must pre-exist, and the diagnostic verifies the external card before
layout preparation or persistence access. A mounted test root is disposable and
must not be the normal product root. The caller creates the outside-root sentinel
before the mounted run. Running the command while unmounted proves the fail-closed
refusal; after validation, controlled accepted/rejected auth runs prove the final
settings transition, byte stability, outside-root sentinel, and actual zero
progress operations. Use `rejected` instead of `accepted` with a controlled
wrong synthetic key. Unmounting and repeating proves post-unmount refusal.

## Deferred

Deactivation, account switching, account deletion, account-entry UI, and Reader
or conflict behavior remain separate work. Activation does not modify bootstrap,
persisted-profile, normal-sync, explicit pull, or explicit push semantics.
