# Profile-Driven Sync Controller Seam

This milestone adds an additive execution path for a borrowed, already resolved
`cn_persisted_sync_profile`. It does not load persistence, enable sync, inspect
storage, change Reader behavior, or change the KOSync operation implementations.

## Boundary

The caller loads the profile and stops on `DISABLED` or any loader failure. A
profile-driven controller receives only a structurally populated profile and
caller-owned runtime inputs. DNS, TLS/CA, trusted-time policy/configuration,
the progress store, and the exact book path remain outside the profile.

The new APIs are:

```c
typedef struct cn_sync_runtime_inputs {
    const cn_dns_config *dns;
    const cn_tls_config *tls;
    cn_kosync_sync_time_policy time_policy;
    const cn_time_config *time;
} cn_sync_runtime_inputs;

cn_sync_controller_result cn_sync_current_book_with_profile(
    const cn_persisted_sync_profile *, const cn_sync_runtime_inputs *,
    cn_progress_store *, const char *document_path);
cn_sync_push_result cn_sync_push_local_current_book_with_profile(
    const cn_persisted_sync_profile *, const cn_sync_runtime_inputs *,
    cn_progress_store *, const char *document_path);
cn_sync_pull_result cn_sync_pull_remote_current_book_with_profile(
    const cn_persisted_sync_profile *, const cn_sync_runtime_inputs *,
    cn_progress_store *, const char *document_path);
```

All pointers are borrowed for the synchronous call. The profile remains owned
by the caller, which clears it with `cn_persisted_sync_profile_clear()` after
execution. No new heap allocation or full profile copy is introduced. The
existing KOSync client copies and clearing behavior remain in place.

## Compatibility

The old controller APIs remain unchanged. Each now performs its existing
settings/credential preflight and constructs an internal borrowed resolved
view. The profile APIs construct the same view directly. Both paths reach the
same operation-specific resolved core, so policy, mutation evidence, retry
classification, PUT/GET asymmetry, and Reader integration remain unchanged.

Profile calls leave `settings_result` and `credential_result` at their existing
invalid/unattempted initialization values. Explicit pull accepts the complete
profile but never consumes or transmits its device ID.

## Validation

```text
bash testapp/build-profile-sync-controller.sh
```

The focused static ARM diagnostic uses linker-wrapped persistence and operation
functions. It directly asserts zero settings and credential loads, exact
resolved client and runtime inputs, one operation per profile call, normal
result semantics, push mutation evidence, pull GET/local-save behavior, and
configuration failures for invalid profile/runtime inputs. Synthetic account
values are not printed or documented.

The diagnostic is suitable for a narrow target smoke using only synthetic
fixtures, but this milestone does not perform physical HTTPS validation.
