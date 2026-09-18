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

## Repository structure

- `testapp/` — hardware test application and host-side integration tests
- `toolchain/` — reproducible ARM build environment
- `diag/` — diagnostic utilities for the device
- `docs/` — implementation notes and research

## Hardware

Target device:

- Barnes & Noble Nook Simple Touch
- 800×600 E Ink display
- ARM Linux

## License

License has not been selected yet.