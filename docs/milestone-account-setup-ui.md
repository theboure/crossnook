# Account Setup UI Integration

Milestone #32 wires the published account controller to a bounded HOME entry
and a dedicated Account Setup screen. The UI sends one-shot intents; the
application coordinator owns verified external storage, layout preparation,
runtime configuration and controller calls.

## UI and controller contracts

`cn_account_setup_inspect()` classifies local account state without identity
creation, network or writes. An enabled settings file takes the existing fast
path before credential loading. Disabled classification clears its temporary
credentials. Entry requests this inspection only. A disabled account retries
through `cn_account_setup_activate_existing()`; explicit correction requires a
separate correction selection and confirmation before `REPLACE_DISABLED`.

The screen has four bounded fields, a small touch/physical-key keyboard, and
bounded secret-free feedback. Password bytes live only in the UI's fixed 1024
byte buffer and are wiped after controller submission and on screen/application
exit. Rendering uses one asterisk per entered password character and does not
render password bytes.
Activation is synchronous: the caller flushes the Working frame before
processing the one-shot application action.

## Device executable

`testapp/crossnook-account-setup-ui` is a focused Nook executable, not a general
startup/settings framework. It requires its activation DNS, SNTP and CA runtime
inputs explicitly:

```text
crossnook-account-setup-ui <font> <mountpoint> <root> <major> <minor> \
  <dns-ip> <dns-port> <sntp-ip> <sntp-port> <ca-file>
```

The coordinator positively verifies the selected mounted device before
preparing storage and re-verifies the same root/device before each controller
action. `/data` and paths beneath it are explicitly rejected. It does not mount
storage or configure Wi-Fi. A network failure is
reported to the user and can be retried explicitly.

## Validation

Run the static ARM build and deterministic QEMU UI gate with:

```text
bash testapp/build-account-setup-ui.sh
```

The gate tests bounded keyboard editing, field navigation, state/action
selection, length-matched password masking without plaintext rendering, wiping,
uncertainty recheck behavior, profile/corrupt fail-closed behavior, 600x800
rendering and absent-mount refusal. `bash testapp/build-ui.sh` remains the UI
regression gate. Controller storage/network matrix tests remain in
`bash testapp/build-account-setup-controller.sh` and
`bash testapp/build-sync-activation.sh`.

Physical validation for milestone #32 has been completed on the Nook Simple
Touch. Automated framebuffer and geometry checks remain regression safeguards;
they do not replace device validation for future UI changes.

## Deferred

Account deletion/deactivation, enabled-account switching or credential
rotation, background retry/scheduling, Wi-Fi management, Reader automatic
sync triggers, general preferences and unrelated UI redesign remain deferred.
