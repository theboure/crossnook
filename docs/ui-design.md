# CrossNook UI design direction

This document is the project-specific visual and interaction source of truth.
Current milestone requirements and validated CrossNook behavior take precedence,
followed by this direction and the existing architecture. General design guidance
is useful only where it is compatible with the hardware and these constraints.

## Product character

CrossNook should feel like a deliberate modern E-Ink product, not a diagnostic
utility, Android settings screen, phone app, or web dashboard. Its visual
character is minimal, calm, functional, typography-led, high-contrast, and
spacious without wasting the limited display.

Modern reMarkable, Kindle, and Kobo reading interfaces are useful references for
calm composition, strong typography, intentional whitespace, clear actions, and
comfortable touch targets. CrossNook must remain visually simpler and more
restrained and must not copy another product pixel-for-pixel.

## Device constraints

- Target a fixed 600x800, 6-inch monochrome or grayscale E-Ink display.
- Use the existing full-frame redraw model. Do not assume partial refresh.
- Repaint only after meaningful state changes.
- Do not use animation, blinking UI, or a blinking cursor.
- Keep ghosting acceptable through stable, high-contrast states and restrained
  filled areas.
- Physical NEXT, PREV, MENU, and BACK navigation is first-class.
- Touch and physical controls must express the same interaction model.
- The interface must remain intentional and understandable without motion.
- Real-device readability and usability take priority over abstract symmetry.

## Hierarchy and typography

- Establish hierarchy through clear differences among screen titles, section
  labels, field values, actions, status text, and navigation hints.
- Use comfortably readable text on the physical panel; avoid tiny labels and
  low-contrast gray body text.
- Keep labels concise and on one line when practical. Give predictable labels
  enough width instead of wrapping them inside cramped columns.
- Bound unpredictable values without allowing them to collide with adjacent
  content. Preserve access to the currently edited portion of long values.
- Use plain action names. Decorative brackets, icons, or technical notation
  must not substitute for hierarchy.

## Layout and spacing

- Use an intentional spacing rhythm: related label/value content stays close,
  while fields, actions, and status regions have visibly larger separation.
- Empty space is useful when it clarifies reading order, but text and controls
  must not remain unnecessarily small while large regions go unused.
- Prefer proximity, simple separators, and selection backgrounds over boxes.
- Use very few borders. A boundary should communicate focus, action, or a
  meaningful control region rather than decorate every row.
- Maintain clear alignment and stable geometry across default, focused, error,
  working, success, and correction-confirmation states.

## Interaction and focus

- Every screen has one obvious primary task or action.
- Touch targets should be generous and should use the available display area.
- Visible control geometry and hit-test geometry must remain aligned exactly.
- Focus and selection must be unmistakable in pure monochrome and must not rely
  on color or subtle gray differences alone.
- Prefer a stable high-contrast fill, strong outline, or similarly explicit
  state that survives E-Ink contrast and ghosting.
- Physical focus order, touch order, and visual reading order must agree.
- Loading, success, failure, and recovery feedback must be explicit and
  secret-free, with no animation required.

## Forms and keyboard

- Forms should not resemble tables or debug configuration panels.
- Prefer a label above its value with a simple separator below when that gives
  labels and values adequate width and improves scanning.
- Field text must be optically and vertically centered within its value region.
- Keep field labels, values, helper/status text, and the primary action visually
  distinct without surrounding every item with a decorative container.
- Show the on-screen keyboard only while editing requires it and use the
  available lower screen area efficiently.
- Keyboard keys need clear boundaries, readable labels, adequate separation,
  and touch geometry identical to their visible geometry.
- Password contents remain secret, but password length is intentionally visible
  through one asterisk per entered character. No plaintext secret may appear in
  rendering, logs, or reports.

## Avoid

- Tiny labels or dense desktop-style forms
- Boxed rows everywhere or decorative cards
- Rounded-card smartphone styling
- Gradients, shadows, and low-contrast gray-on-gray presentation
- Decorative or nonfunctional icons
- Web-dashboard conventions
- Animation, blinking cursors, or subtle transient states
- Focus or meaning communicated by color alone

## Review standard

Review UI work at 600x800 using representative real content and all relevant
states. Automated framebuffer and geometry checks guard regressions, but final
readability, touch comfort, perceived scale, focus clarity, and ghosting require
judgment on a real Nook Simple Touch.
