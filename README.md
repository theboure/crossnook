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
- Library Core: a non-recursive book scanner (EPUB/FB2/TXT by filename)
  and a library-list UI (paging, row selection, Selected Book screen)
  composed by `crossnook-library-test`
- CREngine EPUB Rendering Spike: koreader/crengine (GPL-2.0) opens a real
  EPUB and lays out + renders readable RGB565 pages for the 600x800
  framebuffer, as a fully static non-PIE musl binary (`crossnook-cre-test`;
  host-validated under qemu, see `docs/milestone-crengine-rendering-spike.md`)
- Library → Reader Integration: a reusable reader layer
  (`src/reader/reader.{h,cpp}`) connects the Library UI to CREngine —
  HOME → LIBRARY → select EPUB → reader (page 0) → NEXT/PREV turns
  CREngine pages → BACK returns to the Library selection intact; FB2/TXT
  and broken-EPUB activations fall back to the Selected Book screen
  (`crossnook-reader-test`; host-validated under qemu, see
  `docs/milestone-reader-integration.md`)
- Reader Logical Position / Sync Foundation: the Reader C API can capture,
  own, copy, and restore an opaque logical document location across close /
  reopen and pagination-changing relayouts without making page number the
  canonical position (`crossnook-position-test`; host- and hardware-validated,
  see `docs/milestone-reader-position.md`)
- Local Reading Progress Persistence: a bounded versioned ProgressStore saves
  canonical ReaderPosition records under an explicit state directory and
  restores independent books across process restarts and layout changes
  (`crossnook-progress-test`; host- and hardware-validated,
  see `docs/milestone-local-progress.md`)
- KOReader-Compatible Document Identity: an isolated BookIdentity module
  reproduces KOReader/KOSync Binary partial-MD5 and Filename document keys
  without changing Local Progress `path-v1` records (`crossnook-bookid-test`;
  host- and hardware-validated, see
  `docs/milestone-koreader-identity.md`)
- Wi-Fi Detach + Plain-HTTP: a bounded static ARM HTTP/1.0 client
  (`crossnook-net-test`) and a read-only `diag/network-info.sh` probe back a
  manual association path for the TI wl1251 stack on the diagnostic image;
  the full chain was proven on hardware against a PC HTTP server
  (`NESTEST GET 200 ... OK`, host- and hardware-validated, see
  `docs/milestone-wifi-networking.md`)

## Repository structure

- `testapp/` — hardware test application and host-side integration tests
- `src/` — reusable module tree (platform / graphics / ui / library / reader /
  progress / book),
  composed by `crossnook-ui-test`
- `toolchain/` — reproducible ARM build environment
- `diag/` — diagnostic utilities for the device
- `docs/` — implementation notes and research

See `docs/crossnook-ui-test.md` (UI Core composition diagnostic),
`docs/crossnook-library-test.md` (library composition diagnostic) and
`docs/crossnook-text.md` (FreeType renderer) for details.

## Hardware

Target device:

- Barnes & Noble Nook Simple Touch
- 800×600 E Ink display
- ARM Linux

## License

License has not been selected yet.
