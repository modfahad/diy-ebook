// Enclosure skeleton for the Elecrow CrowPanel ESP32-S3 5.79" E-Paper HMI.
//
// NOTHING HERE HAS BEEN MEASURED. Every dimension below is a named parameter
// with its provenance in a trailing comment. The geometry never contains a
// bare number -- if a value is wrong, it is wrong in exactly one place.
//
//   // datasheet  taken from a published Elecrow/vendor figure
//   // derived    computed from other parameters
//   // GUESS      invented to make the model close; VERIFY WITH CALIPERS
//
// Replace the GUESS values against the real board before printing anything.

/* [What to render] */
// "assembly" for on-screen fit checking, "base" or "front" to export for print.
part = "assembly";  // ["assembly", "base", "front"]

/* [Board] */
pcb_w      = 165.0;  // GUESS -- board outline width  (X)
pcb_h      =  62.0;  // GUESS -- board outline height (Y)
pcb_t      =   1.6;  // GUESS -- bare PCB thickness, standard 1.6 mm stock
tallest_c  =   6.0;  // GUESS -- tallest component behind the board (USB, headers)

/* [Panel] */
// The active area is 792 x 272 pixels. The glass and its inactive margin are
// larger than that, and the offset of the active area within the glass is not
// symmetric on most e-paper panels. Measure all four of these.
glass_w    = 138.0;  // GUESS -- panel glass width
glass_h    =  46.0;  // GUESS -- panel glass height
active_w   = 125.4;  // GUESS -- visible active area width  (792 px)
active_h   =  43.0;  // GUESS -- visible active area height (272 px)
active_dx  =   0.0;  // GUESS -- active area offset from glass centre, X
panel_dx   = -10.0;  // GUESS -- glass centre offset from board centre, X
active_dy  =   0.0;  // GUESS -- active area offset from glass centre, Y
panel_dy   =   6.0;  // GUESS -- glass centre offset from board centre, Y

/* [Mounting] */
mount_hole_d  = 2.6;   // GUESS -- M2.5 clearance
mount_boss_d  = 6.0;   // derived-ish: hole + wall, GUESS until holes are found
mount_inset_x = 4.0;   // GUESS -- hole centre from board edge, X
mount_inset_y = 4.0;   // GUESS -- hole centre from board edge, Y

/* [Shell] */
wall       = 2.4;   // choice -- 3 perimeters at 0.4 mm nozzle
floor_t    = 2.0;   // choice
lid_t      = 2.4;   // choice
fit_gap    = 0.4;   // choice -- clearance around the board on every side
lip_h      = 3.0;   // choice -- how far the front lip drops into the base
lip_gap    = 0.2;   // choice -- print clearance on the lip
corner_r   = 3.0;   // choice -- outer corner radius
bezel      = 3.0;   // choice -- how much frame overlaps the glass edge

/* [Cutouts] */
// Positions are from the centre of the corresponding face. All GUESS.
usb_w      = 9.5;   // GUESS -- USB-C receptacle opening width
usb_h      = 4.0;   // GUESS -- USB-C receptacle opening height
usb_x      = 0.0;   // GUESS -- offset along the left face
usb_z      = 0.0;   // GUESS -- offset from the board plane
encoder_d  = 8.0;   // GUESS -- rotary encoder shaft clearance
encoder_x  = 70.0;  // GUESS -- encoder centre from board centre, X
encoder_y  = 0.0;   // GUESS -- encoder centre from board centre, Y
sd_w       = 13.0;  // GUESS -- microSD slot opening width
sd_h       =  3.0;  // GUESS -- microSD slot opening height
sd_x       =  0.0;  // GUESS -- offset along the right face

$fn = 48;

// ---------------------------------------------------------------- derived --

cavity_w = pcb_w + 2 * fit_gap;                 // derived
cavity_h = pcb_h + 2 * fit_gap;                 // derived
cavity_d = tallest_c + pcb_t;                   // derived
outer_w  = cavity_w + 2 * wall;                 // derived
outer_h  = cavity_h + 2 * wall;                 // derived
base_z   = floor_t + cavity_d;                  // derived
mount_x  = pcb_w / 2 - mount_inset_x;           // derived
mount_y  = pcb_h / 2 - mount_inset_y;           // derived

// ------------------------------------------------------------------ parts --

// A rounded slab centred on X and Y, sitting on z = 0.
module slab(w, h, d, r) {
    linear_extrude(height = d)
        offset(r = r) offset(r = -r)
            square([w, h], center = true);
}

// Screw boss for one board corner, standing on the cavity floor.
module boss() {
    difference() {
        cylinder(d = mount_boss_d, h = tallest_c);
        translate([0, 0, -1]) cylinder(d = mount_hole_d, h = tallest_c + 2);
    }
}

module bosses() {
    for (sx = [-1, 1], sy = [-1, 1])
        translate([sx * mount_x, sy * mount_y, floor_t]) boss();
}

// Side-wall openings. Cut generously through the wall in the axis it faces.
module side_cutouts() {
    cut = wall * 4;
    // USB-C, left face (-X)
    translate([-outer_w / 2, usb_x, floor_t + tallest_c + usb_z])
        rotate([0, 90, 0])
            translate([0, 0, -cut / 2]) slab(usb_h, usb_w, cut, usb_h / 2 - 0.01);
    // microSD, right face (+X)
    translate([outer_w / 2, sd_x, floor_t + tallest_c])
        rotate([0, 90, 0])
            translate([0, 0, -cut / 2]) slab(sd_h, sd_w, cut, sd_h / 2 - 0.01);
    // Rotary encoder shaft, through the front (handled by the front part) and
    // clear of the base wall here so the shaft body is not pinched.
    translate([encoder_x, encoder_y, floor_t])
        cylinder(d = encoder_d, h = base_z);
}

// Lower tray: the board drops in face up, screwed to four bosses.
module base() {
    difference() {
        union() {
            difference() {
                slab(outer_w, outer_h, base_z, corner_r);
                translate([0, 0, floor_t])
                    slab(cavity_w, cavity_h, cavity_d + 1, max(corner_r - wall, 0.1));
            }
            bosses();
        }
        side_cutouts();
    }
}

// Front frame: covers the board, opens over the active area, drops a lip into
// the base so the two halves locate without fasteners.
module front() {
    lip_w = cavity_w - 2 * lip_gap;
    lip_h_ = cavity_h - 2 * lip_gap;
    difference() {
        union() {
            slab(outer_w, outer_h, lid_t, corner_r);
            translate([0, 0, -lip_h]) difference() {
                slab(lip_w, lip_h_, lip_h, max(corner_r - wall, 0.1));
                slab(lip_w - 2 * wall, lip_h_ - 2 * wall, lip_h, 0.1);
            }
        }
        // Window over the active area, minus the bezel overlap.
        translate([panel_dx + active_dx, panel_dy + active_dy, -lip_h - 1])
            slab(active_w - 2 * bezel, active_h - 2 * bezel,
                 lid_t + lip_h + 2, 1.0);
        // Recess for the glass so it sits flush behind the frame.
        translate([panel_dx, panel_dy, -lip_h - 1])
            slab(glass_w + 2 * fit_gap, glass_h + 2 * fit_gap, lip_h + 1, 1.0);
        // Encoder shaft.
        translate([encoder_x, encoder_y, -lip_h - 1])
            cylinder(d = encoder_d, h = lid_t + lip_h + 2);
    }
}

// -------------------------------------------------------------- selection --

if (part == "base")  base();
else if (part == "front") front();
else {
    base();
    translate([0, 0, base_z + lip_h + 15]) front();  // exploded for inspection
}
