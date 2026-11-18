# A lathe: plan notes

Moved from ROADMAP.md phase 14 on Nov 18 2026 (the user's idea, Oct 30 2026); research sources before building.

- **The part as its profile r(z)**, a surface of revolution as CNC lathe simulators keep it: the tool sets r(z) = min(r(z), its own profile); the mesh rebuilt by revolving the profile into rings, normals from dr/dz.
- **Chips** carry the removed volume, pi (r_old² - r_new²) dz a sample, times the density, leaving along the tangent at the surface speed omega r: fragments for brittle materials (particles, then phase 15's convex bodies), later a curling ribbon for ductile metals (a stiff, curled strip on the cloth solver).
- **Optional cutting force:** F = k_c x depth of cut x feed a turn (k_c about 1500-2000 N/mm² for steel, **from memory**: check before writing tests), its power F v slowing the spindle.
- **For parts that do not spin** (milling, carving): the tool subtracted from a signed distance field, d = max(d_part, -d_tool), remeshed by surface nets.
- **Tests:** a profile cut to a set radius revolves into a closed mesh of that radius; the chips' mass equals the removed volume's; a tool clear of the part removes nothing.
