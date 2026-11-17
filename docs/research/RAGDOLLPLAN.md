# Active ragdolls: plan

Notes and a build plan for Euphoria-style active ragdolls in this engine. These are characters whose bodies are simulated, held up by joint motors and kept standing by balance controllers, so a push makes them stagger, step, fall with their hands out and get up again.

The research half (layers 1-6 and the optional learning section) records what the papers and GTA V's exposed NaturalMotion interface say, with their numbers.

Each layer then says **what it means here**:

- what the engine already has;
- what must be built;
- where it lives in `src/`;
- how it is tested.

The roadmap items are in [ROADMAP.md](../ROADMAP.md): layers 1-5 are phase 8, layer 6 is in phase 9 (see "Where it goes in the roadmap"). The finished explanation will go in [ENGINE_GUIDE.md](ENGINE_GUIDE.md).

Started Oct 18, 2026, adapted to the engine the same day.

## The idea in one paragraph

A Euphoria-style character is a physics ragdoll whose joints are driven towards target angles by motors, with a stack of controllers choosing those targets every physics step.

No single trick makes it work. It is six layers, each depending on the one below:

1. **Physics body.** About 13-20 rigid capsules with joint limits, simulated at a small timestep.
2. **Joint actuators.** A PD servo per joint that pulls it towards a target angle, with gains scaled by inertia and a torque cap.
3. **Gravity compensation and virtual forces.** Feed-forward torques that hold the body up, so the PD gains can stay low and the character looks soft.
4. **Balance and stepping.** A world-frame torso controller plus foot placement driven by the centre of mass (COM) position and velocity.
5. **Behaviours and a supervisor.** Balance, stagger, catch fall, protect head, writhe and so on, each with conditions for when it can run, layered by body part and switched as the situation changes.
6. **Animation blending.** Track the animated pose while nothing happens, go soft on impact, react, then blend back.

```
 animation pose / behaviours ──targets, gains, helper forces──▶ supervisor (masks per body part)
                                                                     │
          ┌──────────────── body state (COM, contacts, poses) ◀──┐   ▼
          ▼                                                   │  balance + stepping
     every behaviour reads it back each substep               │      │
                                                              │      ▼
                                                              │  gravity compensation (feed-forward torques)
                                                              │      │
                                                              │      ▼
                                                              │  PD joint drives (torque, capped)
                                                              │      │
                                                              └── rigid-body solver (XPBD substeps, joints, contacts)
```

What GTA V's scripting interface exposes matches this closely:

- stiffness and damping per body part;
- a balancer with about 64 parameters;
- separate behaviours that stack on body-part masks;
- explicit "helper" forces;
- global strength and health scalars that weaken every gain.

The papers explain how each piece works.

## What the engine has and lacks

Today's physics is **kinematic**: the character controller moves a capsule and pushes it out of walls, and particles are points with a radius.

Nothing has an orientation that physics changes, a mass distribution or a joint. The ragdoll needs all of that first.

**Already there and reused**

- **The tick.** Fixed 120 Hz ticks through `runFixedUpdate`, interpolation with `PreviousTransform`, and the rule that input becomes commands handled in the tick. A shove or a bullet hit is a command, like `wantsJump`.
- **Level contact.** `Capsule`, `Sphere`, the static level's grid and `contact(capsule, a, b, c)` (`physics/triangle_contact`): point, normal, depth and face normal of a capsule against a level triangle. This is exactly the ragdoll-against-world contact.
- **Capsule pairs.** `closestPointsBetweenSegments` (`math/closest_points`): capsule-against-capsule contact for self-collision and ragdoll-against-ragdoll, once wrapped as a `contact` overload.
- **Picking.** `Collider` and `CollisionScene`: give each body part a capsule `Collider` and picking, hovering and selection work on ragdolls with no new code.
- **Friction.** `bounceVelocity`'s friction rule (`physics/particle`) and its tests, as the model for contact friction.
- **Debug views.** `DebugDraw::capsule`, `shape`, `sphere` and the F4 readout lines.
- **Units.** Metres, kilograms and seconds throughout, so the papers' newtons and newton-metres apply directly.

**Done since this plan was written** (it listed them as missing)

- **Quaternions** (`quaternions`, phase 6a): `Transform::rotation` is now a `Quat`, and `interpolate` uses slerp. The plan's reasons still explain why:
  - A tumbling body passes through every orientation, including the gimbal-lock poses where Euler angles break down, and blending angles one at a time does not give the in-between orientation.
  - Rigid bodies need a `Quat` (`math/quat`: multiply, conjugate, rotate a vector, from axis-angle, to a matrix, slerp, the log map to an axis-angle vector for joint errors, `toRotationVector`).
  - Phase 9's glTF skins store their rotations as quaternions too. Code that sets a yaw (characters, the camera targets) builds the rotation with `Quat::fromAxisAngle`.
- **The `-O2` release build and the profiler** (phase 5's last items). 15 bodies, 14 joints and their contacts at 1.2 kHz are cheap optimised but a lot of function calls at `-O0`.

**Missing, in the order the ragdoll needs it**

- **Rigid-body dynamics** (once all in the optional phase, now split: phase 8 builds it for the ragdoll): orientation, angular velocity, mass and inertia, joints and contact solving. The ragdoll is the first thing that needs it, so its needs set the solver's shape.
- **An oriented box** does not exist (`Aabb` stays axis-aligned when moved, so it is only a bound). The papers use boxes for the pelvis, chest and feet; here those are built from capsules instead (see layer 1), and box-against-triangle contact stays unbuilt until something needs it.
- **Skeletal animation and the parent/child hierarchy** (phase 9). Only layer 6 and the skinned look need them; layers 1-5 work with hand-written poses and capsule meshes.

## Layer 1: Physics body and simulation

Build a humanoid from about 13-20 primitive shapes, give every joint human limits, and simulate at 1-2 kHz. Everything above depends on this being stable and deterministic.

**What the papers do**

- **Shapes:** capsules for limbs, boxes for the pelvis, chest and feet. Masses from anthropometric tables. DeepMimic's humanoid is a good reference: 13 links, 34 degrees of freedom, 45 kg, 1.62 m.
- **Joints:** hinges for knees and elbows; ball joints for hips, shoulders, spine and neck. Each degree of freedom gets limits from human range-of-motion data.
- **Soft limits:** a spring-damper near each limit rather than only a hard stop. GTA V exposes these as `configureSoftLimit` (stiffness, damping, limit angle).
- **Self-collision:** skip pairs joined by a joint. Separately keep the legs from passing through each other (GTA has `configureSelfAvoidance`).
- **Feet:** one or two segments. Better foot contact makes balance noticeably easier.

| Setting | Values used in the papers |
| --- | --- |
| Physics timestep | 0.3-0.5 ms (ODE), 5 ms (SIMBICON 3D), 1.2 kHz (DeepMimic, Bullet) |
| Control rate | Every physics step for PD; 30 Hz for a learned policy |
| Friction | 0.5-1.0 (0.8 is typical) |
| Restitution | 0 |
| Engines used | ODE, Bullet, PhysX, Havok, MathEngine |

- **Use a robust, deterministic simulation.** Any optimiser (evolution, RL, CMA-ES) finds and exploits physics bugs such as energy leaks; Sims hit this in 1994.
- **Contact quality matters most.** Ground contact strongly affects how well balance controllers work; most foot problems trace back to it.
- **Reduced or maximal coordinates:** reduced coordinates (Featherstone) keep joints exact; maximal coordinates (each body free, joints as constraints) are simpler but joints can drift under high load.

**What it means here**

### The solver

**XPBD rigid bodies with substeps** (Müller et al. 2020, "Detailed Rigid Body Simulation with Extended Position Based Dynamics"), not a library and not Featherstone. Reasons:

- it is the rigid-body version of the solver phase 7 builds for cloth (same substep loop, same compliance idea, so the two share code and understanding);
- joints, joint limits and contacts are all position constraints solved the same way;
- it stays stable with stiff joints at small substeps;
- drift is small because each substep corrects positions directly.

It works in maximal coordinates, so the joint drift the papers warn about is real but shrinks with the substep length.

- **Substeps.** Each tick (1/120 s) runs `RigidBodySettings::substeps` substeps, default 10 (h = 0.83 ms, 1.2 kHz, DeepMimic's rate); try 20 if joints stretch under hard hits. One constraint iteration per substep, as cloth will do (Macklin 2019: small steps beat many iterations).
- **A substep:**
  1. Predict each body:

     v += h F/m, x += h v

     ω += h I⁻¹(τ - ω × Iω), q += (h/2) [ω, 0] q, normalise q

  2. Solve joints, limits and contacts as position corrections.
  3. Derive v = (x - x_prev)/h and ω from the change in q.
  4. A velocity pass for restitution and dynamic friction.
- **A position correction** along unit direction n at offset r from a body's centre uses the generalised inverse mass

  w = 1/m + (r × n) · I⁻¹ (r × n)

  With compliance α (inverse stiffness, α̃ = α / h²) the multiplier step is

  Δλ = (-c - α̃ λ) / (w₁ + w₂ + α̃)

  giving impulse p = Δλ n, applied as x += p/m and q += ½ [I⁻¹ (r × p), 0] q. Angular corrections are the same with w = n · I⁻¹ n.

  These are from memory of the paper: **read Müller 2020 before writing the tests** and add its exact formulas here, as `ILLUMINATION.md` does for DDGI.
- **World inverse inertia.** It is R I_body⁻¹ Rᵀ. With a diagonal body-frame inertia (capsules along an axis) apply it by rotating a vector into the body frame with the quaternion, dividing by the three moments and rotating back, so no 3x3 matrix type is needed.

### Contacts, shapes and joints

- **Contacts against the level:**
  - Once per tick, query the level grid for the triangles near each body's box swept over the tick (padded by its speed times the tick), as `sweepSphere(level, ...)` already does.
  - Then every substep recompute `contact(capsule, a, b, c)` against just those triangles and solve the penetrating ones (normal correction, static friction as a positional correction capped by μ times the normal correction).
  - Restitution 0 and friction 0.8 by default, both in the settings.
  - Sort contacts by (body, triangle) so the order never depends on how the grid stored them.
- **Shapes: capsules only**, so every contact is one `contact` call we already trust.
  - A body part may hold several capsules (a compound in body coordinates): the pelvis and chest are one sideways capsule each.
  - Each **foot is two short parallel capsules** (inner and outer edge) welded into one body, which gives a flat support area instead of the single line a lone capsule rolls on.
  - Oriented boxes and box-against-triangle contact (separating axes) wait until a box-shaped foot is shown to balance better.
- **Joints:** a `Joint` connects two bodies at a frame fixed in each (anchor point plus orientation).
  - A ball joint keeps the anchors together (three position constraints) and limits the relative rotation by **swing-twist**: split the relative quaternion into a twist about the bone's axis and a swing of the axis, then limit the swing to a cone (or an ellipse: different limits forwards and sideways) and the twist to a range.
  - A hinge also keeps one axis aligned in both bodies and limits the angle about it.
  - Soft limits are limit constraints with a compliance instead of zero.
- **Self-collision:** capsule-against-capsule contacts between bodies of the same ragdoll that are not joined, found with the ragdoll's own few bodies (no grid needed for 15), and also between different ragdolls through the uniform grid.

### The body

**Body proportions:** one `RagdollBodySettings` (height, total mass, and per segment a length and mass fraction, radius) whose defaults match the character controller's capsule (1.8 m tall), so a character can hand over to its ragdoll and back. 70 kg total by default.

Starting values come from the usual anthropometric table (Winter, from Dempster's cadaver data; quoted from memory, check before using):

| Segment | Mass fraction | Length (fraction of height) |
| --- | --- | --- |
| Head and neck | 8.1% | |
| Trunk | 49.7% | |
| Upper arm | 2.8% | 0.186 |
| Forearm | 1.6% | 0.146 |
| Hand | 0.6% | 0.108 |
| Thigh | 10% | 0.245 |
| Shank | 4.65% | 0.246 |
| Foot | 1.45% | 0.152 |
| Shoulder width | | 0.259 |
| Hip width | | 0.191 |

**Bodies are entities.**

- Each body part is an entity with `Transform` (centre of mass and orientation), `PreviousTransform`, a `RigidBody` component (mass, body-frame inertia, velocity, angular velocity, its capsules), a `Collider` (picking) and, until phase 9, a `ModelRenderer` showing its capsules.
- Joints are `Joint` components on their own entities naming the two bodies.
- The solver gathers bodies and joints into plain arrays each tick (indices instead of entity lookups inside the substep loop), solves and writes back.
- This keeps rigid bodies general (a crate on a hinge uses the same code) and gives interpolation and picking for free.
- An `ActiveRagdoll` component on the character's root entity lists its bodies by role (pelvis, chest, head, thighs ...) and its joints, and holds controller state. Destroying a ragdoll destroys them all (collected first, never inside `each`).

### Units, drawing, determinism and folders

- **Real numbers, as in the papers** (decided Oct 18, 2026): `RigidBodySettings::gravity` is 9.81, and masses, lengths, torque caps, gains and push forces are the papers' real-world values in SI units.
  - So their results (Coros's 600 N for 0.1 s, SIMBICON's k_p ≈ 300) can be reproduced and compared directly.
  - The character controller keeps its Overwatch 17.5, which is game feel, not physics; a character handing over to its ragdoll keeps its velocity and then falls at 9.81.
- **Drawing:** a `Mesh::capsule(segments, rings)` generator (like `Mesh::sphere`), so each body shows its real shapes: a mannequin look until skinning.
- **Determinism:** fixed tick, fixed substep count, fixed iteration order, no threads, contacts sorted.
  - Same build and same inputs give bit-identical results, tested by running a scene twice.
  - Different builds can differ, since `-O2` may fuse multiply-adds; `-ffp-contract=off` fixes that if networking ever needs lockstep.
- **Folders:**
  - `math/quat`;
  - `physics/rigid_body` (component, prediction, inertia of capsules);
  - `physics/joint` (component, swing-twist, limits);
  - `physics/rigid_body_solver` (the substep loop, contacts, `RigidBodySettings`);
  - a capsule-capsule `contact` in `physics/triangle_contact` (or a new `physics/shape_contact`);
  - the ragdoll builder and all controllers go in `gameplay/ragdoll/`.

## Layer 2: Joint actuators (the muscles)

Every controller here outputs target joint angles, never raw torques. A PD servo per joint turns each target into a torque.

Reil's patent, SIMBICON, Coros, Zordan and DeepMimic all work this way, and it is the most important design decision in the plan.

**The papers**

- **The PD law:**

  τ = k_p (θ_d - θ) + k_d (θ̇_d - θ̇)

  - For 3-degree-of-freedom joints, compute the error as a quaternion difference turned into an axis-angle vector (the log map).
  - Clamp τ to a per-joint limit: Coros used 200 Nm for hips and knees and 100 Nm elsewhere; SIMBICON used 370 Nm.
- **Critical damping:** k_d = 2√k_p (Coros), or k_d = 0.1 k_p (SIMBICON). Start critically damped and adjust.
- **Scale by inertia** (Zordan 2005):

  τ = I (k_p (θ_d - θ) - k_v θ̇)

  with I the moment of inertia of everything beyond the joint. One stiffness number then means roughly the same thing at the neck and the hip.

  GTA V's parameters fit this: stiffness mostly 6-16, and damping 1.0 documented as critical, which suggests stiffness is a natural frequency (rad/s) and damping a damping ratio. This is a reading of the parameters, not something NaturalMotion published.
- **Reference values (SIMBICON 3D):** k_p ≈ 300 on most joints and about 1000 at the waist, for a roughly 70 kg body. Once gravity compensation is in (layer 3), halving or doubling k_p mostly changes how stiff or sloppy the character looks (Coros).
- **Stable PD** (Tan, Liu and Turk 2011) computes the torque from the predicted next state, so high gains stay stable at game timesteps. DeepMimic and AMP use it; its equations were not checked from the sources read.
- **Ways to drive a joint:**
  - your own PD torque (full control);
  - the physics engine's motor (a force-limited velocity constraint, the patent's preferred option);
  - Hill-type muscles (Geijtenbeek 2013, needs offline optimisation; skip).
- **Strength and health.** GTA V lets scripts set stiffness, damping and "muscle stiffness" per body-part mask, plus a global strength (0-1, "dead granny" to "healthy terminator") and health (0-1). Scaling every gain by them gives wounded, drunk and dying characters without a behaviour for each.

**What it means here**

- **A `JointDrive` per joint, in `physics/joint`** (a motorised hinge on a crate is the same thing). It holds:
  - target orientation (relative to the parent body) and target angular velocity;
  - `stiffness` as a natural frequency ω (rad/s) and `damping` as a damping ratio ζ;
  - a torque cap, and the subtree inertia I.

  The torque, with e the log-map error vector, clamped to the cap:

  τ = I (ω² e - 2ζω (θ̇ - θ̇_d))

  This is the inertia-scaled PD with GTA's units: tune one ω for the whole body.
- **Explicit torques first.** Each substep the drive's torque goes into prediction as +τ on the child body and -τ on the parent (an internal torque: equal and opposite, so the drive cannot move the whole body's centre of mass, like a real muscle).
  - At h = 0.83 ms explicit PD is stable far beyond these gains: the limit is about ω_eff h < 2, and even if a light hand sees 20 times the subtree inertia ratio, ω = 16 gives ω_eff h ≈ 0.06.
  - Gravity compensation and balance torques add into the same slot, which a constraint motor would not allow.
- **Implicit drive as the fallback:** if some joint needs very high gains, express the drive as an XPBD angular constraint towards the target with compliance α = 1 / (I ω²).
  - That is implicit like stable PD, so it never blows up, and its torque cap is a clamp on the multiplier (|Δλ| ≤ τ_max h² per substep).
  - Only build it if explicit drives misbehave.
- **The subtree inertia I** is a scalar per joint: the sum over every body beyond the joint of its moment about the joint (parallel axis theorem: I_body + m d²), computed once from the rest pose (Zordan's simplification). Recompute per tick only if poses that fold the limbs feel wrong.
- **`RagdollStrength`** (strength and health, 0-1) multiplies every drive's ω² and cap; behaviours write per-mask multipliers on top. All defaults in one `JointDriveSettings` (ω 10, ζ 1, caps 200 / 100 Nm) so tuning happens in one place.

## Layer 3: Gravity compensation and virtual forces

Gravity compensation makes the character look alive instead of stiff. Without it the PD gains must be high just to hold the body up, so every reaction looks robotic.

Coros showed that removing it makes the torso slouch and reaching fail.

**The papers**

- **Virtual force by the Jacobian transpose:** to make a body part feel force F, apply τ = Jᵀ F to every joint in the chain between a fixed base and that part. In practice, for each joint on the chain:

  τ_joint += (p - joint_position) × F

  with p the point the force acts on.
- **Gravity compensation (Coros 2010):** for every link i outside the stance leg, a virtual force -m_i g at its centre of mass, mapped through the chain from the pelvis to link i and added to every joint between. A carried crate adds its weight at the hand.
- **Gate it on balance:** GTA V's animPose has a "ZMP gravity compensation" switch that scales compensation to zero when the COM leaves the support area. A falling character should go limp, not hold its pose in mid-air.
- **Velocity control by a virtual COM force:**

  F_V = k_V (V_d - V)

  through the chain from the stance ankle to the head; Coros used k_V = 100 Ns/m. Zero the ankle part once the heel lifts.
- **Helper forces ("Hand of God"):** external forces on the pelvis or chest that are not physically real, used sparingly to keep the character upright or guide a fall (GTA: stayUpright, resistAcc, rollDownStairs' helper torques). Keep them small and fade them out as the character fails, so the cheat never shows.

**What it means here**

- **The physics of it.** In physics terms, Jᵀ F is the principle of virtual work: the joint torques that do the same work as F would through any small motion of the chain.

  Since each joint torque is applied as an equal and opposite pair, the "virtual force" moves the limb as if pushed while the body as a whole gets no external push: only the ground contact can supply that, as in a real body.
- **One function.** `virtualForceTorques(ragdoll, chain, point, force)` in `gameplay/ragdoll/` adds into the same per-joint torque slots the drives use; gravity compensation and the COM velocity force are calls to it. Tested on a two-link arm against the torque computed by hand.
- **The gate.** The support area for gating is the convex hull of the foot contact points from the solver's contact list (projected onto the ground plane); the gate is a smooth factor from 1 well inside to 0 outside, in `GravityCompensationSettings`.
- **Helper forces** are real external forces on a body, added at prediction, with a per-ragdoll `HelperForceSettings` budget that behaviours fade.

## Layer 4: Balance and stepping

Balance comes from three things working together:

- keep the torso upright in world space;
- place the next foot from where the COM is heading;
- decide when balance has failed.

These come from SIMBICON (2007) and Coros (2010), and GTA's `configureBalance` parameters line up with them.

**1. Torso control in the world frame (SIMBICON's key trick).**

PD-control the torso's orientation against a world-up target, not relative to the pelvis, giving τ_torso. Do the same for the swing thigh, giving τ_swing.

The stance hip gets no target of its own; its torque is whatever gives the torso the right net torque:

τ_stance = -τ_torso - τ_swing

Only internal torques, no external force on the root.

**2. Foot placement from COM feedback.**

SIMBICON adjusts the swing hip's target in both the sagittal (front-back) and coronal (side-to-side) planes:

θ_d = θ_d0 + c_d d + c_v v

with d the horizontal distance from the stance ankle to the COM, v the COM velocity.

- Starting values c_d = 0.5, c_v = 0.2 in both planes worked in 3D; stable 3D sagittal ranges c_d -0.71 to 1.4 and c_v 0.03 to 0.59.
- Too little c_v and speed builds until it falls; too much and it rocks harder each step until it falls.

Coros treats the body as an inverted pendulum and computes where the foot must land to stop it:

d = v √(h/g + v²/(4g²)), then d' = d - 0.05 V_d

- Clamp d' to 0.6 × leg length so a hard push becomes several steps rather than one huge one.
- Move the swing foot towards the target along a spline, and solve hip and knee angles with analytic two-bone IK.

**3. A small state machine.**

- **SIMBICON walk:** 4 states mirrored in pairs; one timed (about 0.3 s, swing hip 0.5 rad, swing knee -1.1 rad), the next ends on foot contact.
- **Coros step:** a phase φ = t/T with spline targets per joint; a new step starts on foot strike or when t ≥ T.
- **Standing:** hold the COM over the midpoint between the feet; start stepping when the COM nears the edge of the support polygon or its speed passes a threshold; stop when speed is near zero at foot strike.

**4. Failure detection.**

GTA's `configureBalance`:

- `balanceAbortThreshold` (how far it may lean);
- `giveUpHeight` (how low the COM may drop);
- `maxSteps` and `maxBalanceTime`;
- `predictionTime` (how far ahead the COM is predicted when choosing a step);
- `stepHeight` (0-0.4 m);
- `legStiffness` (6-16);
- lean scaled by floor acceleration (vehicles, platforms);
- `fallType` (ramp stiffness down, keep stepping, or slump).

When balance fails, hand over to a fall behaviour rather than keep stepping; Faloutsos found early failure detection is what makes behaviours connect cleanly.

**Robustness to expect.**

- SIMBICON's 3D walker survived 0.4 s chest pushes of about 190-340 N.
- Coros's 70 kg character survived 600 N for 0.1 s from any direction at any point in the step.

**What it means here**

- **Coros's pendulum placement over SIMBICON's gains:**
  - it has a physical derivation (the capture point of an inverted pendulum, a physics reader can check it);
  - it works from standing as well as walking;
  - it needs no per-gait tuning of c_d and c_v.

  Keep SIMBICON's world-frame torso and stance-hip solve, which both papers share.
- **Settings.** `BalanceSettings` holds GTA's knobs with Coros's numbers as defaults (prediction time, abort lean, give-up height, max steps, step clamp 0.6 leg lengths, leg stiffness ω). `stepHeight` defaults to the character controller's 0.3 so a ragdoll steps up the same kerbs a character can.
- **One shared state.** The COM, its velocity, the support polygon and stance and swing feet come from one `RagdollState` computed at the start of each substep from the bodies and the solver's contacts; every controller reads it, nothing recomputes it.
- **Two-bone IK** (`gameplay/ragdoll/two_bone_ik`, law of cosines for the knee, then the hip aims the leg) is pure geometry, tested alone. Phase 9 will reuse it for feet on uneven ground in animation.
- **Moving floors:** the level never moves today, so the floor-acceleration lean waits for moving platforms.
- **The demo's obstacle course is the test track:** standing on the 20 degree ramp, being pushed off the 0.25 steps, the doorway's wall to fall against.

## Layer 5: Behaviours and the supervisor

Behaviours are what make it feel like Euphoria. Each is a small controller that writes PD targets, gains and helper forces for some of the body; a supervisor decides which run and hands control from one to the next.

The design comes from Faloutsos 2001, and GTA V's message system shows NaturalMotion built it the same way.

**What each behaviour exposes (Faloutsos)**

- `canHandle(state)`: a score; 0 unsuited, 1-10 can do something sensible from here, 10-20 sure to reach its goal.
- **Pre-conditions** (balance: COM speed under 0.3 m/s, COM over the support polygon, both feet down).
- **Post-conditions** (the same, tightened).
- **Expected performance** (a self-check while running, to report failure early).
- `update()` writing targets, gains and forces for the body parts it owns.

**Supervisor.**

When the active behaviour succeeds or fails, or something forces a change (a hit, a script command), score every behaviour and run the highest.

No transition graph is written by hand: Faloutsos got balance, protective step, fall, roll, get up, stand without one, and a push during get-up sends the character back to falling.

Hand-written pre-conditions are coarse. Faloutsos learned them by applying random 0.3 s pushes, recording success and training a classifier (an SVM): 97.7% accuracy for walking.

**Layering by body-part mask (GTA V).**

Each behaviour message carries a body-part mask (full body, upper body, one limb), and several run at once. ScriptHookVDotNet's demo starts armsWindmillAdaptive and then bodyBalance on top, so the arms flail while the legs balance.

Each behaviour owns a mask; higher-priority behaviours override lower ones only on the joints they own.

**GTA V's behaviour set** (83 helpers in ScriptHookVDotNet)

| Category | Behaviours |
| --- | --- |
| Body state | setStiffness, setMuscleStiffness, bodyRelax, setCharacterStrength, setCharacterHealth, configureSoftLimit, configureSelfAvoidance |
| Following animation | activePose, animPose, incomingTransforms |
| Balance and stepping | configureBalance, bodyBalance, staggerFall, teeter, yanked, stayUpright |
| Hit reactions | shot (sub-messages for wounds, spin, fall to knees), configureBullets, applyImpulse, upperBodyFlinch, electrocute, onFire |
| Falling and ground | catchFall, highFall, smartFall, rollDownStairs, bodyRollUp, bodyFoetal, bodyWrithe, injuredOnGround, fallOverWall, braceForImpact |
| Arms, head, weapon | armsWindmill, armsWindmillAdaptive, headLook, pointArm, pointGun, grab |
| Lean primitives | lean, hipsLean, forceLean (in a direction, to a position, towards an object) |

A useful first set: activePose, balance, staggerFall, catchFall, armsWindmill, headLook, bodyWrithe and a get-up; that covers most of what players notice.

Suggested internals (a design, not documented by NaturalMotion):

- catchFall aims the hands at the predicted ground contact and lowers arm stiffness on impact;
- headLook is a PD target on the neck towards a point;
- teeter leans the hips away from an edge;
- grab reaches a hand to the nearest ledge, then attaches it with a joint.

**What it means here**

- **A behaviour** is a small class in `gameplay/ragdoll/behaviours/` with `score(const RagdollState&)` and `update(const RagdollState&, RagdollTargets&, seconds)`.
  - `RagdollTargets` holds per joint a target, ω, ζ and an owner priority, plus helper forces.
  - The supervisor resets it each tick, runs the active behaviours lowest priority first, and a write only lands where the writer's priority is at least the owner's.
  - Masks are bit sets of body roles.
- **The first polymorphic gameplay code.** This is the engine's first polymorphic gameplay code (a virtual base class with one subclass per behaviour). That is the natural fit here: the supervisor holds a list of behaviours it does not know the types of. The behaviour set is fixed per character type, so it is built once at spawn, not created per tick.
- **Rates.** Behaviours run once per tick (120 Hz), not per substep: targets change slowly and the drives interpolate nothing. Only drives, gravity compensation and the solver run per substep.

  (The papers run controllers every physics step; check that 120 Hz targets do not visibly lag in stepping, and move the balance controller into the substep loop if they do.)
- **Commands into the ragdoll** (shove, hit, "look at", strength changes) are queued by the game in `onInput`/`onUpdate` and consumed by the next tick, the same rule as `wantsJump`. Read-only state (balanced, stepping, fallen, on the ground) is exposed for game code, such as a death check.
- **Pre-conditions:** hand-written first; the learned classifier is an optional later step (see the learning section: the headless simulation makes collecting push trials easy).
- **Handover with the character controller:**
  - A character normally walks as today's kinematic capsule.
  - A big enough hit (a setting) hides its `ModelRenderer`, spawns the ragdoll posed to match with the controller's velocity, and the supervisor takes over.
  - When get-up succeeds, the capsule comes back at the pelvis's ground position and the ragdoll goes away.
  - Until skinning (phase 9), the ragdoll's capsules are the visible body.

## Layer 6: Blending with animation and hit reactions

Most of the time the character should just play its animation. Physics takes over when something happens, and control blends back once it is over.

The recipe comes from Zordan & Hodgins (2002) and Zordan et al. (2005); GTA V's activePose and animPose work the same way.

**1. Tracking the animation (powered ragdoll).**

Feed the animated skeleton in every frame as PD targets (GTA's incomingTransforms and activePose): high stiffness when calm, lower when hurt. Gravity compensation lets tracking stay soft without sagging.

**2. On impact (Zordan 2005).**

1. Drop stiffness on the parts hit (or the whole body) so they yield.
2. Apply the shaped impulse. GTA's configureBullets spreads it over neighbouring parts, adds a delay and a counter-impulse, and multiplies it when airborne or on one leg; that looks better than one raw impulse.
3. Wait 0.1-0.2 s before switching targets: shorter looks like the hit was anticipated, longer looks stunned.
4. Track a slerp towards a recovery pose or clip, with ease-in and ease-out, using the inertia-scaled PD; or hand control to balance or stagger.
5. Bring stiffness back over a few hundred milliseconds. SIMBICON raises gains about 150 ms after an impact to mimic a reflex.

**3. Picking the clip to return to (Zordan 2005).**

Forward-simulate a passive ragdoll briefly, then search the animation library for the frame best matching where the body is heading:

D = Σ_i w_i Σ_b (w_pb ‖p_b - p'_b‖ + w_θb ‖θ_b - θ'_b‖)

over a 0.1-1.0 s window with the roots aligned at its start.

- Weights are heavy on the trunk and the part touching the ground, light on the limbs, largest for early frames.
- Search a pre-reduced set of distinct frames, then refine around the best; the search was about 70% of the cost in the paper, so keep the library small.

**4. Returning to animation.**

Interpolate the root-position error linearly across the transition and slerp joint rotations with linear time weights, then switch tracking off.

For a character on the ground, match the get-up animation whose first frame is closest (face-up or face-down is the main split), then blend into it.

**Known failure cases:** repeated hits in quick succession, sustained contact such as being held, and interpenetration under very large forces. Plan for them from the start.

**What it means here**

- **Before phase 9** there are no clips.
  - "Animation" is a `RagdollPose` (a target orientation per joint) written by hand or generated: stand, T-pose, brace, foetal, the get-up key poses, and the stepping state machine's spline targets.
  - Steps 2 and 5 of the impact recipe and the get-up all work with poses; step 3's clip search does not exist yet.
- **After phase 9**, a glTF skeleton maps onto the ragdoll by a table from bone to body role.
  - Each tick the clip's pose becomes `RagdollTargets` (activePose).
  - Each frame the skinned mesh is drawn from the bodies' interpolated transforms (each bone follows its body, bones without a body follow the animation).
  - Clip search and blend-back come then, with a small library.
- **Shaped impulses** are a function from (hit point, direction, total impulse, `BulletImpulseSettings`) to impulses on the hit body and its neighbours over the next few ticks. Testable alone: total momentum delivered equals the requested impulse plus the counter-impulse.
- **What delivers hits:**
  - phase 12's hitscan weapons raycast the `CollisionScene`, which already finds a body's capsule `Collider`, so a hit knows the body part;
  - grenades only collide with the static level today (phase 12 adds sweeps against moving colliders);
  - for the demo before then, a shove command from the existing picking ray.

## Optional: learned controllers

Machine learning is not needed: layers 1-6 are hand-designed. Learning helps with two jobs: tuning parameters offline, and replacing a hand-built behaviour with a trained one.

### Evolution: where Euphoria started (Reil & Husbands 2002, Reil's patent)

A small continuous-time recurrent network (CTRNN):

τ_i dy_i/dt = -y_i + Σ_j w_ji o_j, o_i = 1 / (1 + e^(θ_i - y_i))

- **Stepping:** Euler steps of 0.02 s, motor neuron outputs (0-1) mapped onto joint ranges as PD targets.
- **Search ranges:** w in [-16, 16], τ in [0.5, 5], θ in [-4, 4].
- **Genetic algorithm:** population 50-100, top half replaces bottom half, Gaussian mutation of about one gene per individual, no crossover.
- **Fitness:** distance travelled by the hindmost foot (penalises a single lunge), ending when the COM drops.
- **Scaffolding was essential:** a spring holds the trunk up early and weakens as fitness improves.
- **Symmetry:** reuse identical mirrored modules for left and right (half the parameters).
- **Sensing:** these walkers were open-loop; Reil added sensing by evolving sensor weights on top of a frozen walking network.

### Offline tuning with CMA-ES (Geijtenbeek 2013)

Population 20, σ = 1. Minimise:

| Term | Weight |
| --- | --- |
| Speed error | 100 |
| Head orientation | 10 |
| Head velocity | 10 |
| Foot sliding | 10 |
| Effort | 0.1 |

A practical way to tune balance gains and behaviour parameters.

### Reinforcement learning (DeepMimic 2018, AMP 2021)

- **Actions** are PD targets for every joint through stable PD, policy at 30 Hz, physics at 1.2 kHz.
- **State:** each link's position, rotation and velocity relative to the pelvis in a heading-aligned frame, plus a phase.
- **DeepMimic's reward:** imitation 0.7 and task 0.3, the imitation part 0.65 pose + 0.1 velocity + 0.15 end-effector + 0.1 COM, each exp(-k error²).
- **Two tricks are essential:** start episodes at random points of the clip, and end them when the head or torso touches the ground (backflip score 0.791 with both, 0.730 or 0.379 without one).
- **Push recovery** appears without being designed (the walk survived 240 N forwards and 300 N sideways for 0.2 s).
- **AMP** replaces the tracking reward with a discriminator scoring how human the motion looks:

  r = max(0, 1 - 0.25 (D - 1)²)

- **Cost:** about 60M samples and 2 days on 8 cores per DeepMimic skill, 100-300M for AMP, retrained for each body.

**What it means here**

- **The simulation already runs headless**, so a tuning tool is cheap: a second executable (`tools/` or a makefile target like the tests) that builds a world with a flat level and a ragdoll, applies scripted pushes and scores the result, with no window or GPU.
  - CMA-ES over `BalanceSettings` is the one learning step worth planning, after layer 4 works.
  - The determinism test from layer 1 comes first, or the optimiser tunes noise.
- The same tool collects Faloutsos's push trials for learned pre-conditions.
- RL stays out of scope: it needs a neural network training stack the engine does not have, and millions of samples per skill.

## Where it goes in the roadmap

Decided Oct 18, 2026, and written into `ROADMAP.md` (later phases renumbered):

- **Phase 8, "Active ragdolls", right after cloth (phase 7), holds layers 1-5.**
  - By then phase 5's release build and profiler exist and phase 7 has built the XPBD substep loop the rigid-body solver extends with rotation and inertia.
  - Layers 1-5 need no animation: they use hand-written poses, and the body is drawn as its capsules (a mannequin).
- **It was to start with the quaternion switch** (`Transform::rotation` to `Quat`), in its own branch, so phase 9's glTF skins find quaternions already there; that switch was done early, in phase 6a.
- **Layer 6 is an item in phase 9 (animation)**, next to skeletal animation, which it needs: clip poses as drive targets, skinned characters drawn from the bodies, and blending back after a hit.
- **Phase 14's rigid-body item is split:** the solver, joints and capsule contacts come with the ragdolls in phase 8; oriented boxes, stacking and particle-against-particle collisions stay in phase 14 as extensions of the same solver.
- Phase 12's FPS hit reactions and deaths then use the ragdolls.

## Build order

Bottom up; do not start a layer until the one below is solid. One branch per step, tests written first, and each step ends with something visible in the demo:

- spawn a ragdoll with a free key such as R in front of the camera;
- shove with E along the view ray using the existing `pick`;
- F4 draws each body's capsules, joint frames, the COM, the support polygon, the next foot target and a readout line with the supervisor's active behaviours.

The steps:

1. **`quaternions`** (done in phase 6a): `math/quat` and `Transform::rotation` as a `Quat`, slerp in `interpolate`.
   - Tests: rotation round trips, slerp halfway, the log map of a known rotation, `getMatrix` unchanged for the old yaw-only uses.
   - Visible: nothing changes (the point).
2. **`rigid-bodies`:** `RigidBody`, prediction with gyroscopic torque, capsule inertia, contacts with the level, `Mesh::capsule`.
   - Tests: free flight conserves momentum and angular momentum; a spinning capsule about its middle axis tumbles (the tennis-racket theorem, a good gyroscopic check); a dropped capsule comes to rest without jitter or sinking; the same run twice is bit-identical.
   - Visible: R drops a capsule onto the obstacle course.
3. **`joints`:** ball and hinge joints, swing-twist limits, soft limits, self-collision; the ragdoll builder from `RagdollBodySettings`.
   - Tests: a two-body pendulum keeps its joint together to within a small drift and its period matches theory for small swings; limits hold; jointed pairs never collide.
   - Pass: dropped from a height, the limp ragdoll falls down the steps and settles with no jitter, explosions or joints drifting apart.
4. **`joint-drives`:** inertia-scaled PD with caps, strength and health.
   - Tests: one hinge reaches its target critically damped (no overshoot at ζ = 1) in about the time ω predicts; the torque never exceeds the cap; momentum of the whole ragdoll is unchanged by drives alone.
   - Pass: hung by the pelvis (a joint to the world), it holds a T-pose, and one ω feels consistent across the body.
5. **`gravity-compensation`:** `virtualForceTorques`, gating on the support area.
   - Tests: the two-link arm's torques match hand calculation; with compensation and no gravity error, a pose holds at low ω.
   - Pass: lower ω until the pose barely holds without compensation, switch it on; the pose holds softly and a shove makes it yield and recover.
6. **`standing-balance`:** world-frame torso, stance-hip solve, COM over the feet.
   - Pass: it stands unaided on flat ground and on the 20 degree ramp, and absorbs small shoves without stepping.
7. **`stepping`:** capture-point foot placement, two-bone IK, the step state machine.
   - Test (headless, also a regression test for later tuning): a 0.1 s push of 300 N at the chest leaves it standing 3 s later.
   - Pass: shoves of a few hundred newtons give one or more recovery steps.
8. **`falling`:** failure detection, catchFall, fading compensation and helper forces.
   - Pass: a large shove gives a stagger, then a fall with the hands out.
9. **`behaviour-supervisor`:** scoring, masks, priorities, armsWindmill, headLook, get-up, the handover with the character controller (the MOBA duck could be the first character that falls over and gets back up).
   - Pass: the arms windmill while the legs keep stepping; a shoved character gets up and walks on.
10. **In phase 9, after skeletal animation, `ragdoll-animation`:** skeleton-to-body mapping, activePose tracking, skinned drawing from bodies, blend-back, shaped impulses, writhe.
    - Pass: a walk clip plays physically, a hit makes the character react, and it returns to the clip with no visible pop.
11. **Optional:** the headless tuning tool with CMA-ES over `BalanceSettings`.

**Pitfalls the papers ran into**

- **Hiding bad compensation:** high gains to hide bad compensation make everything look robotic: fix the compensation instead.
- **Blow-ups:** ordinary PD at high gains or large timesteps blows up: use substeps (done here), stable PD or implicit drives.
- **Torso frame:** pelvis-relative torso control falls over: torso targets must be in world space.
- **Late failure:** detecting failure late makes characters keep walking after balance is already lost: hand off to a fall behaviour early.
- **Exploits:** optimisers exploit physics bugs: make the simulation deterministic before tuning anything automatically.
- **SIMBICON's documented weak spots:** stairs (no look-ahead, so toes stub), flight phases, and a small stable region for one-leg balance. The obstacle course's steps will show the first.

## Sources

Read in full for the original notes, except Zordan & Hodgins 2002 (abstract and project page). Some tables in the Reil & Husbands PDF did not extract, so its body dimensions come from the patent.

**Not yet read for these notes:** Müller et al. 2020, Tan et al. 2011 and Winter's tables, whose formulas and numbers above are from memory and must be checked before tests are written.

- [Reil & Husbands 2002, Evolution of Central Pattern Generators for Bipedal Walking](https://users.sussex.ac.uk/~philh/pubs/reilhusbandsIEEE.pdf)
- [Reil, US7685081 Bipedal walking simulation](https://patents.google.com/patent/US7685081)
- [Sims 1994, Evolving Virtual Creatures](https://www.karlsims.com/papers/siggraph94.pdf)
- [Geijtenbeek & Pronost 2012, Interactive Character Animation Using Simulated Physics](https://perso.liris.cnrs.fr/nicolas.pronost/Papers/Interactive%20Character%20Animation%20Using%20Simulated%20Physics%20-%20A%20State-of-the-Art%20Review%20[Geijtenbeek2012].pdf)
- [Faloutsos, van de Panne & Terzopoulos 2001, Composable Controllers](https://www.cs.ubc.ca/~van/papers/2001-siggraph-composable.pdf)
- [Zordan & Hodgins 2002, Motion Capture-Driven Simulations that Hit and React](https://www.ri.cmu.edu/publications/motion-capture-driven-simulations-that-hit-and-react)
- [Zordan et al. 2005, Dynamic Response for Motion Capture Animation](https://www.researchgate.net/publication/220184339_Dynamic_response_for_motion_capture_animation)
- [Yin, Loken & van de Panne 2007, SIMBICON](https://www.cs.ubc.ca/~van/papers/2007-siggraph-simbicon.pdf)
- [Coros, Beaudoin & van de Panne 2010, Generalized Biped Walking Control](https://www.cs.ubc.ca/~van/papers/2010-TOG-gbwc/paper.pdf)
- [Geijtenbeek, van de Panne & van der Stappen 2013, Flexible Muscle-Based Locomotion](https://www.ics.uci.edu/%7Erickl/courses/cs-271/lit/2013-TOG-MuscleBasedBipeds.pdf)
- [Peng et al. 2018, DeepMimic](https://arxiv.org/abs/1804.02717)
- [Peng et al. 2021, AMP](https://arxiv.org/abs/2104.02180)
- [ScriptHookVDotNet (GTA V NaturalMotion message wrappers)](https://github.com/scripthookvdotnet/scripthookvdotnet)
- [citizenfx/natives (GTA V ragdoll and NM natives)](https://github.com/citizenfx/natives)
- Müller, Macklin, Chentanez, Jeschke & Kim 2020, Detailed Rigid Body Simulation with Extended Position Based Dynamics (the rigid-body solver)
- Macklin, Storey, Lu, Terdiman, Chentanez, Jeschke & Müller 2019, Small Steps in Physics Simulation (substeps over iterations; also cited for cloth in phase 7)
- Tan, Liu & Turk 2011, Stable Proportional-Derivative Controllers
- Winter, Biomechanics and Motor Control of Human Movement (anthropometric segment table, from Dempster)
