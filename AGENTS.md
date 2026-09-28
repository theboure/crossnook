# Agent guidance

## UI design skills

For UI work, use this source-of-truth order: (1) current task and milestone requirements, (2) existing CrossNook architecture and code, (3) CrossNook UI/device constraints in the repository, (4) UI/UX Pro Max recommendations, then (5) Impeccable recommendations. CrossNook hardware, e-ink, architecture, performance, and milestone constraints always override generic skill guidance.

Use `ui-ux-pro-max` to plan user-facing screens, interaction/navigation, layout, hierarchy, typography, spacing, component behavior, or substantial UI implementation. Treat web/mobile advice as suggestions. Reject assumptions incompatible with the actual device, including hover, web-only technology, gratuitous or continuous animation, GPU-heavy effects, unsuitable gradients/effects, and input patterns the device does not support.

Use `impeccable` primarily as a bounded post-implementation design review for hierarchy, spacing, typography, controls, visual consistency, and accessibility/usability. Do not redesign unrelated screens, introduce frameworks, or override CrossNook constraints.

Do not invoke either design skill for work limited to synchronization, networking, storage, backend/core logic, build/toolchain, device communication, tests, or unrelated infrastructure.
