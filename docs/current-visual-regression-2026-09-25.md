# Current-Build Physical Visual Regression

**Validation date: 2026-09-25**

This note records human visual evidence for the current integrated build. It
is separate from historical milestone validation and distinguishes physical
screen observation from framebuffer, ADB, and diagnostic evidence.

## Baseline and artifacts

- Commit: `8161ad081af31cef8093f66a6c164bfb0a9095ce`
- FreeType artifact: `crossnook-text`
- FreeType artifact SHA-256:
  `4750d503224df2e0b2187217b7c052e5e991af5367f275e3144819b25f941269`
- Font: `test-font.ttf`
- Font SHA-256:
  `7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954`
- Reader artifact: `crossnook-reader-test`
- Reader artifact SHA-256:
  `540f8235c5e08ff4f326a0379175fc8ba6a4841d9dd723dae960ac1adee629fb`
- EPUB fixture: `valid.epub`
- EPUB fixture SHA-256:
  `7824ae76297051d40a49eaa224e13e6dbdbae09af595355c47ff2e1f4944ba67`

The diagnostics used `/tmp` on the Nook. No `/data` state and no networking
were used for this local visual regression.

## FreeType

Target execution: **PASS**. `crossnook-text` rendered a 600x800 frame to
`/dev/graphics/fb0` using 960000 bytes and then slept.

Supporting framebuffer evidence was:

- before visible render:
  `d7ae01851d068916e937394337127c37e92f9dba01f8a567b53aa526c8330e13`;
- while the human was observing the rendered text:
  `4ba45d9c36a222903b18918dbfffefae0997640891228db3a2a272ec6`.

These hashes support that framebuffer contents changed, but are not visual
proof. The human observed on the physical panel:

- `CrossNook` and `DejaVu Sans`;
- a visible and legible Cyrillic pangram;
- visible 18 / 24 / 32 / 48 px samples;
- the complete 48 px sample within the display;
- no right- or bottom-clipping of the large sample; and
- an actual physical E-Ink repaint.

Visual physical: **PASS**. The post-tag FreeType layout correction was
visually confirmed.

## Integrated Reader

Target execution: **PASS**.

The initial physical screen showed `CrossNook`, `UI Core`, `Open Reader Test`,
and `Open Library`. Physical E-Ink rendering was confirmed. After the human
tapped `Open Reader Test`, the panel showed:

```text
Reader Test
Page One
Съешь ещё этих мягких французских булок, да выпей чаю
```

The page was readable and nonblank, and the touch marker was visibly rendered.

### Page navigation

The physical right-upper side key (NEXT) was pressed. The displayed marker
changed from `Page One` to `Page 2` on the physical E-Ink panel.

- Reader NEXT navigation: **PASS**
- Reader NEXT visual update: **PASS**

The physical left-upper side key (PREVIOUS) was pressed. The displayed marker
changed from `Page 2` to `Page 1` and the physical page update was confirmed.

- Reader PREVIOUS navigation: **PASS**
- Reader PREVIOUS visual update: **PASS**

There was faint residual E-Ink ghosting from older content. The requested page
remained visible and readable; old content did not replace or obscure it, and
the panel was neither blank nor conspicuously corrupted.

### Exit

The physical right-lower side key (BACK) was pressed. The panel visibly
returned to the UI Core screen. Residual Reader ghosting was not strongly
visible and did not interfere with the UI.

- Reader BACK / exit visual: **PASS**
- UI Core visual: **PASS**
- Reader initial page visual: **PASS**

## Classification

```text
FreeType visual physical: PASS
UI Core visual: PASS
Reader initial page visual: PASS
Reader NEXT visual: PASS
Reader PREVIOUS visual: PASS
Reader BACK visual: PASS
Overall current visual regression: PASS
E-Ink full-refresh / ghosting policy: NOT YET VALIDATED
```

This evidence proves that the physical panel visibly updates to the requested
Reader states. It does not validate a full-refresh or partial-refresh policy,
waveform behavior, or a general E-Ink ghosting policy.
