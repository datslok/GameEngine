# Orbital simulation: plan notes

Moved from ROADMAP.md phase 13 on Nov 18 2026; no sources gathered yet: research them before building.

## Precision

- A float keeps about 7 significant digits: at Neptune's distance (4.5 billion km) positions snap in steps of hundreds of kilometres; a double's 16 digits give about a millimetre. `DVec3` for simulation positions; float `Vec3` for rendering; phase 5's particle functions (`fly`, `bounceVelocity`) templates over the vector type.
- Camera-relative rendering in double (a floating origin): the subtraction in double before the small difference becomes a float, so the GPU sees numbers near zero.
- Depth: reversed-Z first; a logarithmic depth or split near and far passes if a nearby spacecraft and a planet millions of kilometres behind still do not both sort.

## Simulation

- **Symplectic integrator** (leapfrog / velocity Verlet): ordinary integrators pump energy into or out of orbits; symplectic ones keep the energy error bounded. Test: total energy in a small band over many orbits.
- **Time warp:** many substeps a 120 Hz tick; a step varied naively breaks the symplectic guarantee, so a time-symmetric step choice or fixed substeps only for bodies in a close pass.
- **On rails:** distant bodies on their exact Kepler ellipse (no integration error, almost no cost), switched to full simulation when the player comes near.
- **Barnes-Hut** (or the fast multipole method) for thousands of bodies (an asteroid belt): a distant cluster pulls as one mass at its centre, O(n²) to O(n log n); drawn instanced.
