# Sync Account Bootstrap Core

This milestone adds local-only KOSync account/configuration bootstrap. It
does not register or validate an account remotely, enable sync, start normal
sync, load the persistent device identity, or modify KOSync, Reader, settings,
credentials, storage verification, or existing diagnostics.

## Boundary

The caller supplies a `cn_storage_layout` that has already passed portable
storage verification and `cn_storage_layout_prepare()`. The bootstrap module
does not discover mounts, select a root, create the root, create layout
directories, touch internal eMMC, or perform network activity.

It uses the existing stores at:

```text
<verified-root>/config/settings.conf
<verified-root>/state/credentials
```

The storage modules remain the only persistence framework. Their strict format,
CRC, corruption behavior, atomic save behavior, and typed durability results
are preserved.

## Password and Userkey

The bootstrap API accepts a caller-owned raw password pointer and explicit
length. It never modifies or claims to erase that caller buffer. The chosen
CrossNook API bound is:

```c
CN_ACCOUNT_PASSWORD_MAX 1024
```

The existing MD5 API accepts an arbitrary `size_t` length and does not define
an upstream KOReader password maximum. Therefore 1024 is an explicit bounded
CrossNook API/product limit, not a claimed KOReader restriction. Passwords over
the bound are rejected and never truncated.

The documented KOReader-compatible derivation is used exactly:

```text
userkey = lowercase hexadecimal MD5(password bytes)
```

The existing RFC 1321 implementation in `src/book/md5.[ch]` is reused. The
known vector is:

```text
password: test-password
userkey:  dfb450efddbb5387197c84460623675b
```

Only the username and derived userkey are sent to the existing credential
store. The raw password is never persisted, logged, returned, or placed in
settings. Internal digest, userkey, and credential copies are cleared with a
volatile byte wipe where practical. This is memory hygiene, not a secure
deletion guarantee.

The userkey is an account-equivalent secret. Diagnostics never print it or a
display digest of it. FAT/VFAT mode bits do not provide credential
confidentiality on removable media.

## Bootstrap State Machine

All input validation, layout/store initialization, existing settings load,
existing credential load, and password derivation occur before any write.

If existing valid settings contain `kosync_enabled=true`, bootstrap returns a
dedicated `existing-active` result before any write. This API is not an account
switch workflow. Active-account switching is deferred.

Missing settings and credentials are acceptable. Existing malformed or
unsupported files are hard failures and are never replaced.

Semantically equal valid records are not rewritten. The report contains only
status fields and:

```text
settings_written=0|1
credentials_written=0|1
```

For changed or missing state:

1. Save desired settings with `kosync_enabled=false`.
2. Only after settings success, save credentials.

The settings save uses the requested URL and device name. A settings failure
or `DURABILITY_UNCERTAIN` prevents credential save. A credential failure or
durability uncertainty leaves settings disabled and does not roll back visible
state. Such a state is explicitly incomplete/fail-closed. No transaction
framework or completion-marker file is introduced.

A future activation flow must not infer account readiness merely because both
files exist. Activation requires explicit successful bootstrap/activation
semantics and remains outside this milestone.

## API

The report contains no credential-bearing fields:

```c
typedef struct cn_account_bootstrap_report {
    cn_account_bootstrap_result result;
    cn_account_bootstrap_stage stage;
    cn_settings_result settings_result;
    cn_credential_result credential_result;
    cn_kosync_userkey_result userkey_result;
    int settings_written;
    int credentials_written;
} cn_account_bootstrap_report;
```

The main operation is:

```c
cn_account_bootstrap_report cn_account_bootstrap(
    const cn_storage_layout *layout,
    const char *base_url,
    const char *device_name,
    const char *username,
    const unsigned char *password,
    size_t password_length,
    int *system_errno);
```

Typed results distinguish invalid input, an existing active configuration,
existing settings failure, existing credential failure, derivation failure,
settings write failure, credential write failure, and durability uncertainty.
The stage identifies whether durability uncertainty occurred during settings
or credentials.

## Host Validation

Run:

```text
bash testapp/build-account-bootstrap.sh
```

The diagnostic uses compiled synthetic values and accepts no credential command
line arguments. It covers:

- KOReader MD5 vector validation.
- Valid bootstrap and disabled settings.
- Reopening settings and credentials.
- Caller password buffer preservation.
- Raw password and userkey absence from persisted artifacts and output.
- Identical repeated bootstrap with zero writes.
- Settings-only change.
- Credential-only change.
- Existing active configuration refusal before writes.
- Missing/invalid/oversized input.
- Malformed settings and credentials.
- Settings write failure.
- Settings durability uncertainty preventing credential write.
- Credential failure leaving reloadable disabled settings.
- No network, DNS, TLS, HTTP, or KOSync linkage.

## Physical Diagnostic

The diagnostic accepts only storage verification arguments:

```text
/tmp/crossnook-account-bootstrap-test --physical <mountpoint> <root> <major> <minor>
```

It uses synthetic compiled fixtures and no credential arguments. With the card
unmounted it refuses before persistence. After the caller verifies the known
external card, an isolated prepared root beneath that verified mount can be
used, for example:

```text
/tmp/crossnook-card/crossnook-account-bootstrap
```

The current storage verifier supports this: the supplied application root must
be beneath the supplied mountpoint and on the expected device. The root must
already exist; the diagnostic may prepare its layout only after successful
verification. This keeps normal `/tmp/crossnook-card/crossnook/config` and
`state/credentials` untouched.

Physical validation should run the diagnostic twice, reload both stores,
confirm `enabled=false`, confirm raw synthetic password absence, check an
outside-root sentinel, and unmount before repeating the refusal. No network or
real KOSync account is required.

## Deferred Work and Limitations

- Remote account registration and auth probing are deferred.
- Runtime wiring of the persistent device identity is deferred.
- Sync activation UI and account-switch workflows are deferred.
- Account rotation and recovery are deferred.
- The 1024-byte password bound is a CrossNook API choice, not an upstream
  KOReader claim.
- MD5 is used for KOReader protocol compatibility, not password hardening.
- The stored userkey is plaintext account-equivalent material on the card.
- FAT/VFAT power-loss and confidentiality guarantees remain limited.
