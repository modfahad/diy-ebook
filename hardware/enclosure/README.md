# Enclosure

A two-part printed case for the Elecrow CrowPanel ESP32-S3 5.79" E-Paper HMI:
a tray the board screws into, and a front frame with a window over the panel.

```
case.scad          the source -- every dimension is a parameter at the top
models/            exported meshes (case-base.stl, case-front.stl)
preview.png        exploded render, regenerated from case.scad
```

## This has not been measured

**No dimension in `case.scad` came from a real board.** The panel is in the
device and no one has put calipers on it, so the numbers are placeholders
chosen to be internally consistent -- the parts fit each other, not the
hardware. Print one and it will not fit.

Every value lives in a labelled block at the top of the file and carries its
provenance:

| Tag | Meaning |
|---|---|
| `// datasheet` | from a published vendor figure |
| `// derived` | computed from other parameters |
| `// choice` | a print/design decision, not a measurement (wall thickness, clearances) |
| `// GUESS` | invented -- **verify before printing** |

The geometry contains no bare numbers, so correcting a measurement is a
one-line edit. What still needs measuring: board outline and thickness, glass
size and its offset within the board, active-area offset within the glass,
mounting hole positions, tallest rear component, and the USB-C / microSD /
encoder positions.

## Editing and exporting

Open `case.scad` in OpenSCAD; the parameter blocks show up in the Customizer.
`part` selects what renders -- `"assembly"` for an exploded fit check,
`"base"` or `"front"` to export one printable part.

```bash
openscad -o models/case-base.stl  -D 'part="base"'  case.scad
openscad -o models/case-front.stl -D 'part="front"' case.scad
openscad -o preview.png --imgsize=1000,700 --viewall --autocenter case.scad
```

On Windows the binary is at `C:\Program Files\OpenSCAD\openscad.exe`.

The two halves locate on a lip with no fasteners between them; the board is
held by four bosses tapped for M2.5. If the real board has a different hole
pattern, `bosses()` is the only module to change.
