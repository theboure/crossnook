# Persisted Sync Profile Core

This milestone composes the already persisted settings, credentials, and
installation device identity into a bounded owned profile. It is local and
read-only. The caller supplies an initialized and prepared storage layout;
this module does not discover or verify mounts, prepare directories, touch
internal eMMC, enable sync, or perform any network, Reader, book, progress,
DNS, TLS, CA, or trusted-time operation.

## State model

`DISABLED` is returned for missing settings, because the Settings Store
supplies disabled defaults, and for valid settings with `kosync_enabled=false`.
The profile stops at that point and does not inspect credentials or identity.
Corrupt or unsupported settings are typed settings failures.

`READY` means that enabled settings, credentials, and an existing persistent
device identity were all loaded and validated. The profile owns bounded copies
of base URL, username, userkey, device ID, and device name. READY does not
assert network, DNS, TLS, CA, trusted-time, or Reader readiness.

The report contains attempted flags. Underlying results for a component whose
flag is zero are `UNATTEMPTED` by convention: the enum field retains its
ordinary invalid initial value and the flag is authoritative. In particular,
the disabled path reports credentials and identity as unattempted.

## APIs

```c
cn_persisted_sync_profile_report cn_persisted_sync_profile_load(
    const cn_storage_layout *prepared_layout,
    cn_persisted_sync_profile *output);

void cn_persisted_sync_profile_clear(cn_persisted_sync_profile *profile);
```

The report retains the exact Settings Store, Credential Store, and device
identity result for an attempted component. Temporary credentials and the
complete output profile are cleared on failures using the project’s volatile
byte-wipe convention. This is memory hygiene, not a claim of perfect secure
deletion.

## Device identity loading

`cn_device_identity_load()` reuses the existing strict read-only parser. It
returns `OK`, `NOT_FOUND`, malformed, unsupported-version, checksum, and
filesystem results exactly as the existing parser produces them. It performs
no entropy access, serialization, creation, or rewrite. The existing
`cn_device_identity_load_or_create()` API and creation semantics are unchanged.

## Validation

`bash testapp/build-persisted-sync-profile.sh` builds a static ARMv5TE EABI5
soft-float non-PIE diagnostic, checks that the profile object has no forbidden
transport or write references, and runs the deterministic host matrix under
QEMU. The diagnostic supports separate physical fixture setup and composition:

```text
/tmp/crossnook-persisted-sync-profile-test --physical-setup \
  <disabled|ready|missing-identity> <mountpoint> <root> <major> <minor>

/tmp/crossnook-persisted-sync-profile-test --physical-compose \
  <mountpoint> <root> <major> <minor>
```

Both modes verify the caller-selected external storage first. Setup may
prepare the supplied disposable root and create only synthetic fixtures.
Compose initializes the existing layout without preparing it, then calls only
the production profile loader and clears its output. It does not save, remove,
or create persistence. The modes are intended for independent disposable
roots beneath `/tmp/crossnook-card`, not the normal CrossNook product root.
Physical validation is not run by this milestone implementation session.
