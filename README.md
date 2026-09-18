# CrossNook

CrossNook is a lightweight native Linux interface for the Barnes & Noble Nook Simple Touch.

It works directly with the Linux framebuffer and input devices and is intended as an alternative lightweight environment for the device.

## Current status

CrossNook is in early development.

Currently implemented:

- ARM cross-compilation toolchain
- framebuffer rendering
- hardware button input
- touchscreen input
- host-side QEMU integration testing
- execution on real Nook Simple Touch hardware
- FreeType text rendering (UTF-8, Cyrillic, anti-aliasing)
- UI Core module tree (`src/`): framebuffer, input, graphics, text, and a
  minimal UI state layer composed by `crossnook-ui-test`

## Repository structure

- `testapp/` — hardware test application and host-side integration tests
- `src/` — reusable module tree (platform / graphics / ui), composed by
  `crossnook-ui-test`
- `toolchain/` — reproducible ARM build environment
- `diag/` — diagnostic utilities for the device
- `docs/` — implementation notes and research

See `docs/crossnook-ui-test.md` (UI Core composition diagnostic) and
`docs/crossnook-text.md` (FreeType renderer) for details.

## Hardware

Target device:

- Barnes & Noble Nook Simple Touch
- 800×600 E Ink display
- ARM Linux

## License

License has not been selected yet.