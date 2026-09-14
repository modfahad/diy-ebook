# Shared types

**Status: not needed yet — deliberately empty.**

The intent was a package of types shared between the desktop app, the tools and
the firmware's mirrored definitions. In practice
[`@quran-device/qpk-format`](../qpk-format/README.md) already exports every
content type that crosses a boundary today — package and section ids, record
shapes, metadata keys — and `@quran-device/converter` exports the conversion
report and the Quran import schema.

Adding a third package to re-export them would create a dependency edge with no
content behind it.

Revisit when Milestone 4 lands the desktop↔device protocol: request and
response shapes are genuinely shared between the Tauri app and the firmware's
HTTP handlers, and they do not belong in the package format. Until then this
directory stays empty on purpose rather than by omission.
