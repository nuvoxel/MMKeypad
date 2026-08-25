// T3_RemovalTool.scad — replica of the Control4 factory fork tool that
// releases a T3 In-Wall touch screen from its power box. Human-editable source
// of the same profile gen_stl.py emits. Profile reverse-engineered from a
// ruler-calibrated photo of the real steel tool and verified by overlay.
// Render:  openscad -o T3_RemovalTool.stl T3_RemovalTool.scad
//
// Two tall OUTER prongs slide up the wall behind the screen to release the two
// bottom latch tabs; the central pocket clears the centre boss. Overall ~130 x
// 92 mm. Real part is ~1 mm steel — see README for the print thickness/fit
// caveat. +x right, +y measured DOWN from the prong tips.

thickness = 1.2;   // mm — try this first (close to the 1 mm steel)

outline = [
    [2,8],[8,0],[18,0],[18,30],[38,30],[38,21],[41,21],[41,40],[89,40],
    [89,21],[92,21],[92,30],[112,30],[112,0],[122,0],[128,8],[128,66],
    [103,66],[103,93],[27,93],[27,66],[2,66]
];

// y is measured downward in the table above; mirror to standard +y-up so the
// print sits the natural way, then extrude.
linear_extrude(height = thickness)
    polygon([ for (p = outline) [p[0], -p[1]] ]);
