/*
  Space-Saving Snap-Fit Enclosure
  Inner Dimensions: 51 x 21 x 26 mm
  Electronics Size: 51 x 21 x 24 mm
  
  Cover is located on the 51x21 face (Top face).
  Designed for minimal space occupation using thin walls and a snap-fit lid without screws.
*/

// --- Adjustable Parameters ---

// Wall thickness (1.2mm = 3 perimeters with 0.4mm nozzle for minimal space)
wall = 1.2; 
// Thickness of the top cover plate
cover_thickness = 1.2; 
// Depth of the inner lip for the cover (deep enough to catch side screws)
cover_lip_depth = 3.5;

// Inner Dimensions
in_x = 51.5;
in_y = 21;
in_z = 29;

// Rounded Corners
corner_r = 0.6; // Corner radius in the X-Y plane (set to 0 for sharp corners)

// Type-C Hole (Left 21x26 face: -X)
type_c_w = 9.2;
type_c_h = 3.2;
// Position of the Type-C hole (Relative to the inner center of the face)
type_c_y_center = 0; 
type_c_z_center = 24; // Z distance from the inner bottom

// Opposite side holes (Right 21x26 face: +X)
hole_r = 1.2;
// Position of the center hole (Relative to the inner center of the face)
holes_y_center = 0;
holes_z_center = 6.5; // Z distance from the inner bottom
// Spacing between adjacent holes (Total of 3 holes)
holes_spacing_y = 4.6; // Spacing along the Y axis
holes_spacing_z = 0.0; // Spacing along the Z axis

// Tolerance for cover inner lip
tol = 0.15;

// Relay Module dimensions
relay_x = 43.0;
relay_y = 17.0;
relay_z = 5.0; // Depth of the secure fit area
relay_tol = 0.2; // Tolerance for fitting the relay module

// Cover LED Holes
led_r = 1.2; // Radius of the LED holes
// Position of the center LED hole (Relative to the cover center)
leds_x_center = -23; // X position along the long edge (51mm)
leds_y_center = 0; // Y position along the short edge (21mm)
leds_spacing = 8.0; // Distance between adjacent LEDs (parallel to short edge)

// Type-C PCB Supports
// Top of Type-C socket is at: type_c_z_center + type_c_h/2
// Bottom of PCB is at: Top of Type-C socket - 4.25
type_c_pcb_z = type_c_z_center + type_c_h/2 - 4.25; 
pcb_support_size = 4.0; // Size of the corner supports (X and Y dimensions)

// Cover Fixing Screws
screw_r = 1; // Radius for side fixing screws (e.g., M2 or M2.5)
screw_z_offset = 1.8; // Distance from the top edge for the screw holes

// Wall Mount Bracket
add_wall_mount = true; // Include a detachable wall mount bracket
mount_rail_width_base = 7.0; // Dovetail rail base width
mount_rail_width_top = 10.0;  // Dovetail rail top width
mount_rail_thickness = 2.0;   // Dovetail rail thickness
mount_rail_clearance = 0.2;   // Clearance for sliding fit

// --- Modules ---

module rounded_cube(size, r) {
    if (r <= 0) {
        cube(size, center=true);
    } else {
        x = size[0];
        y = size[1];
        z = size[2];
        safe_r = min(r, x/2, y/2);
        hull() {
            translate([-x/2 + safe_r, -y/2 + safe_r, 0]) cylinder(r=safe_r, h=z, center=true, $fn=30);
            translate([ x/2 - safe_r, -y/2 + safe_r, 0]) cylinder(r=safe_r, h=z, center=true, $fn=30);
            translate([-x/2 + safe_r,  y/2 - safe_r, 0]) cylinder(r=safe_r, h=z, center=true, $fn=30);
            translate([ x/2 - safe_r,  y/2 - safe_r, 0]) cylinder(r=safe_r, h=z, center=true, $fn=30);
        }
    }
}

module support_pillar(x_sign, y_sign, z_top, size) {
    // Creates a "half square" (triangular) pillar starting from the inner bottom
    // without occupying the relay PCB sitting area
    cx = x_sign * in_x/2;
    cy = y_sign * in_y/2;
    
    linear_extrude(height=z_top)
    polygon(points=[
        [cx, cy],
        [cx - x_sign*size, cy],
        [cx, cy - y_sign*size]
    ]);
}

module enclosure_box() {
    union() {
        difference() {
            // Outer body
            translate([0, 0, (in_z)/2 - wall/2])
                rounded_cube([in_x + 2*wall, in_y + 2*wall, in_z + wall], r=corner_r + wall);
            
            // Inner cavity
            translate([0, 0, in_z/2 + 0.1])
                rounded_cube([in_x, in_y, in_z + 0.2], r=corner_r);
            
        // Type-C hole (-X face) (Pill shape)
        translate([-in_x/2 - wall/2, type_c_y_center, type_c_z_center])
            rotate([0, 90, 0])
            hull() {
                translate([0, (type_c_w - type_c_h)/2, 0]) cylinder(r=type_c_h/2, h=wall + 0.5, center=true, $fn=30);
                translate([0, -(type_c_w - type_c_h)/2, 0]) cylinder(r=type_c_h/2, h=wall + 0.5, center=true, $fn=30);
            }
            
        // Three Holes (+X face)
        // Center hole
        translate([in_x/2 + wall/2, holes_y_center, holes_z_center])
            rotate([0, 90, 0])
            cylinder(r=hole_r, h=wall + 0.5, center=true, $fn=50);
            
        // Side hole 1
        translate([in_x/2 + wall/2, holes_y_center - holes_spacing_y, holes_z_center - holes_spacing_z])
            rotate([0, 90, 0])
            cylinder(r=hole_r, h=wall + 0.5, center=true, $fn=50);
            
        // Side hole 2
        translate([in_x/2 + wall/2, holes_y_center + holes_spacing_y, holes_z_center + holes_spacing_z])
            rotate([0, 90, 0])
            cylinder(r=hole_r, h=wall + 0.5, center=true, $fn=50);
            
        // Cover Fixing Screw Holes (on the 21x29 faces, +X and -X)
        translate([-in_x/2 - wall/2, 0, in_z - screw_z_offset])
            rotate([0, 90, 0])
            cylinder(r=screw_r, h=wall + 0.5, center=true, $fn=20);
            
        translate([in_x/2 + wall/2, 0, in_z - screw_z_offset])
            rotate([0, 90, 0])
            cylinder(r=screw_r, h=wall + 0.5, center=true, $fn=20);
        }
        
        // Relay Holder Frame (5mm depth secure place)
        translate([0, 0, relay_z/2])
            difference() {
                // Outer frame (1.5mm wall thickness)
                cube([relay_x + 2*relay_tol + 3, relay_y + 2*relay_tol + 3, relay_z], center=true);
                // Inner cutout for the PCB
                cube([relay_x + 2*relay_tol, relay_y + 2*relay_tol, relay_z + 0.1], center=true);
            }
            
        // Corner Ledges to support the PCB (leaving 1.5mm space underneath for solder joints/pins)
        translate([0, 0, 1.5/2]) {
            translate([(relay_x)/2 - 1, (relay_y)/2 - 1, 0])
                cube([2, 2, 1.5], center=true);
            translate([-(relay_x)/2 + 1, (relay_y)/2 - 1, 0])
                cube([2, 2, 1.5], center=true);
            translate([(relay_x)/2 - 1, -(relay_y)/2 + 1, 0])
                cube([2, 2, 1.5], center=true);
            translate([-(relay_x)/2 + 1, -(relay_y)/2 + 1, 0])
                cube([2, 2, 1.5], center=true);
        }
        
        // Type-C PCB Corner Supports (Half-square pillars from Z=0)
        support_pillar(-1, -1, type_c_pcb_z, pcb_support_size); // Front-Left (-X, -Y)
        support_pillar(-1,  1, type_c_pcb_z, pcb_support_size); // Back-Left  (-X, +Y)
        support_pillar( 1, -1, type_c_pcb_z, pcb_support_size); // Front-Right(+X, -Y)
        support_pillar( 1,  1, type_c_pcb_z, pcb_support_size); // Back-Right (+X, +Y)
        
        // Dovetail rail for wall mount (on the Back Y face)
        if (add_wall_mount) {
            translate([0, in_y/2 + wall, (in_z)/2 - wall/2])
                linear_extrude(height=in_z + wall, center=true)
                polygon(points=[
                    [-mount_rail_width_base/2, 0],
                    [ mount_rail_width_base/2, 0],
                    [ mount_rail_width_top/2, mount_rail_thickness],
                    [-mount_rail_width_top/2, mount_rail_thickness]
                ]);
        }
    }
}

module cover() {
    difference() {
        union() {
            // Top plate
            translate([0, 0, cover_thickness/2])
                rounded_cube([in_x + 2*wall, in_y + 2*wall, cover_thickness], r=corner_r + wall);
                
            // Inner alignment lip (Deepened to hold side screws)
            translate([0, 0, -cover_lip_depth/2])
                rounded_cube([in_x - tol*2, in_y - tol*2, cover_lip_depth], r=max(0, corner_r - tol));
        }
        
        // LED Holes (Row parallel to the short Y-edge)
        // Center LED
//        translate([leds_x_center, leds_y_center, 0])
//            cylinder(r=led_r, h=10, center=true, $fn=30);
            
        // Side LED 1
        translate([leds_x_center, leds_y_center - leds_spacing, 0])
            cylinder(r=led_r, h=10, center=true, $fn=30);
            
        // Side LED 2
        translate([leds_x_center, leds_y_center + leds_spacing, 0])
            cylinder(r=led_r, h=10, center=true, $fn=30);
            
        // Cover Fixing Screw Pilot Holes (in the cover lip)
        translate([-in_x/2, 0, -screw_z_offset])
            rotate([0, 90, 0])
            cylinder(r=screw_r * 0.8, h=5, center=true, $fn=20);
            
        translate([in_x/2, 0, -screw_z_offset])
            rotate([0, 90, 0])
            cylinder(r=screw_r * 0.8, h=5, center=true, $fn=20);
    }
}

// --- Layout for 3D Printing ---

// 1. Box body
translate([0, 0, wall])
    enclosure_box();

// 2. Cover (Placed next to the box, flipped upside down for easy printing)
translate([0, in_y + 2*wall + 10, cover_thickness])
    rotate([180, 0, 0])
    cover();

module wall_mount_bracket() {
    bracket_w = in_x + 10;
    bracket_h = in_z + wall + 2.0; // Add 2mm for the bottom stop
    bracket_thickness = mount_rail_thickness + 3.0; // 3mm base plate + rail thickness
    
    difference() {
        // Main bracket body
        translate([0, bracket_thickness/2, (in_z)/2 - wall/2 - 1.0])
            rounded_cube([bracket_w, bracket_thickness, bracket_h], r=corner_r + wall);
            
        // Dovetail slot cutout
        // We add clearance for a smooth sliding fit
        translate([0, 0, (in_z)/2 - wall/2 + 1.0])
            linear_extrude(height=in_z + wall + 1.0, center=true)
            polygon(points=[
                [-(mount_rail_width_base/2 + mount_rail_clearance), -0.1],
                [ (mount_rail_width_base/2 + mount_rail_clearance), -0.1],
                [ (mount_rail_width_top/2 + mount_rail_clearance), mount_rail_thickness + mount_rail_clearance],
                [-(mount_rail_width_top/2 + mount_rail_clearance), mount_rail_thickness + mount_rail_clearance]
            ]);
            
        // Wall mounting screw holes
        translate([-bracket_w/2 + 8, bracket_thickness/2, (in_z)/2 - wall/2])
            rotate([90, 0, 0])
            cylinder(r=2.0, h=bracket_thickness + 2, center=true, $fn=20);
            
        translate([bracket_w/2 - 8, bracket_thickness/2, (in_z)/2 - wall/2])
            rotate([90, 0, 0])
            cylinder(r=2.0, h=bracket_thickness + 2, center=true, $fn=20);
            
        // Screw head countersinks on the front face (Y=0)
        translate([-bracket_w/2 + 8, 1.5, (in_z)/2 - wall/2])
            rotate([90, 0, 0])
            cylinder(r=4.0, h=3.1, center=true, $fn=20);
            
        translate([bracket_w/2 - 8, 1.5, (in_z)/2 - wall/2])
            rotate([90, 0, 0])
            cylinder(r=4.0, h=3.1, center=true, $fn=20);
    }
}

// 3. Wall Mount Bracket
if (add_wall_mount) {
    translate([0, -in_y - 30, mount_rail_thickness + 3.0])
        rotate([-90, 0, 0])
        wall_mount_bracket();
}
