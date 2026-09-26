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

### Physical Validation Evidence

Physical validation was completed on a Nook Simple Touch. The external storage
and test artifacts were:

```text
external card: /dev/block/mmcblk1
sysfs dev: 179:16
verified mount: /tmp/crossnook-card
filesystem: vfat rw
ARM artifact: testapp/crossnook-sync-activation-test
ARM artifact SHA256: ae92846270f86648f1ffbc19aa54ab11500b13a8badb783ac87b0b1e1603af13
test CA on Nook: /tmp/crossnook-testca.crt
test CA SHA256: 003b795b4d19ba55b0ee2b2075bb4283299981132871a56cfb40bef197cec509
```

The controlled network setup was:

```text
DNS mock: 192.168.0.107:19153
SNTP mock: 192.168.0.107:19123
HTTPS/KOSync mock: secure.test.local:18443
configured KOSync base URL: https://secure.test.local:18443/kosync
HTTPS mock base path: /kosync
```

The Nook network checks passed:

```text
NETTEST DNS ROUTE host=secure.test.local address=192.168.0.107 server=0
NETTEST HTTPS DNS GET 200 23 184 OK
TIMETEST QUERY OK
TIMETEST SYNC OK
```

The pre-mount and post-unmount storage gate refused persistence:

```text
SYNC ACTIVATION physical storage=unverified persistence=not-attempted
RC=1

CROSSNOOK_CARD_NOT_MOUNTED
SYNC ACTIVATION physical storage=unverified persistence=not-attempted
RC=1
```

Rejected and accepted cases used separate disposable roots. They MUST NOT share
one root: physical fixture seeding is conditional, so rejected credentials could
otherwise persist into the accepted case.

The rejected clean root was:

```text
/tmp/crossnook-card/crossnook-sync-activation-30-rejected-clean
```

The first rejected run produced:

```text
SYNC ACTIVATION physical status=auth-rejected
auth_get=1
auth_ok=0
saved=0
visible=0
progress_get=0
progress_put=0
credentials_stable=1
identity_stable=1
sentinel_stable=1
enabled=0
RC=0
```

The rejected settings hash before the repeat and after the identical repeat was
`2ee002503757218e18cd13f2fcbac1dbc21990182ecb9911674c3a8482b79f47`. The mock
transcript contained exactly these two redacted events after both runs, with no
progress GET or PUT:

```text
{"authenticated":false,"endpoint":"auth","method":"GET"}
{"authenticated":false,"endpoint":"auth","method":"GET"}
```

The accepted clean root was:

```text
/tmp/crossnook-card/crossnook-sync-activation-30-accepted-clean
```

The accepted run produced:

```text
SYNC ACTIVATION physical status=ok
auth_get=1
auth_ok=1
saved=1
visible=1
progress_get=0
progress_put=0
credentials_stable=1
identity_stable=1
sentinel_stable=1
enabled=1
RC=0
```

The enabled settings hash was
`27f000f972c9edccd7f594999b20aa23ca9d6cda0a5a9c3d6cb28f978f48c139`. The
outside-root sentinel SHA256 was unchanged before and after at
`c1ff99ebff3eabdb913fade983dccef3d741ee0413100fac67c000d03d64a43a`.
The accepted transcript contained exactly one successful auth event:

```text
{"authenticated":true,"endpoint":"auth","method":"GET"}
```

Repeating activation on the already-enabled root returned before networking or
saving:

```text
SYNC ACTIVATION physical status=already-enabled
auth_get=0
auth_ok=0
saved=0
visible=0
progress_get=0
progress_put=0
credentials_stable=1
identity_stable=1
sentinel_stable=1
enabled=1
RC=0
```

The settings hash remained
`27f000f972c9edccd7f594999b20aa23ca9d6cda0a5a9c3d6cb28f978f48c139`, and the
transcript remained exactly one successful auth event.

The physical run also found a test-infrastructure defect. Production correctly
generated `<base-path>/users/auth`, but the mock recognized only `/users/auth`,
so the prefixed physical request returned HTTP 404 and was reported as a service
failure. Test-only base-path routing was fixed in commit `d660ff5`; the
production ARM artifact SHA256 above did not change. The `build-https.sh` gate
remains blocked by its pre-existing PKI reproducibility preflight and is not a
milestone #30 regression.

The evidence proves:

- The verified external-storage gate runs before persistence.
- Rejected remote auth cannot enable sync.
- Rejected auth leaves settings byte-stable.
- Accepted remote auth performs the final enable save.
- Credentials and identity remain stable.
- The outside-root sentinel remains stable.
- Activation performs zero progress GET/PUT operations.
- Repeated activation of an already-enabled profile performs no auth/network operation and no settings save.
- Post-unmount persistence is refused.

**PHYSICAL VALIDATION: PASS**

## Deferred

Deactivation, account switching, account deletion, account-entry UI, and Reader
or conflict behavior remain separate work. Activation does not modify bootstrap,
persisted-profile, normal-sync, explicit pull, or explicit push semantics.
