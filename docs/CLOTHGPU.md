# Cloth on the GPU: research and plan

Notes and a build plan for simulating large garments on the GPU: tens to hundreds of thousands of points, nearly inextensible fabric, folds and layers that never pass through each other, at real-time rates. This is what the dressmaking game (phase 10) needs, and what phase 7's milestones ask of the solver.

The research half records what the papers and NVIDIA's open-source code say, with their numbers. Each part then says **what it means here**. The plan at the end proposes the build order and the roadmap's new items.

Started Oct 25, 2026, after the CPU solver (XPBD, `src/cloth/`) reached its limits on the cloth lab's 100 x 100 sheet.

**Sources, read by five researchers in parallel on Oct 25, 2026:**

- the papers, as full text where it could be fetched;
- NVIDIA's Newton engine source (`newton-physics/newton`, main at `f121272e`, Oct 24 2026), whose VBD solver is the one production implementation;
- the older NVIDIA Warp VBD integrator it grew from (Warp 1.6 and 1.7);
- the Augmented VBD authors' demo code.

Tags: **[read]** is stated in a source, **[inf]** is our inference, **[check]** must be checked against the source before writing tests. Section, equation and file references point to where each claim came from.

## The problem in one paragraph

The engine's cloth today is XPBD with small steps: 15 substeps a 120 Hz tick, one Gauss-Seidel pass over coloured constraints per substep. Each pass carries a correction about one triangle further across the mesh. A large, fine garment has hundreds of triangles between a pin and its hem, so the stiffness never reaches across within a step, and the cloth sags and stretches more the finer its mesh. We measured it: a strip on a mesh twice as fine droops 75% more at 15 substeps (`cloth_bending_test`, `testCantilever`). This is a **low-frequency error**: smooth, sheet-wide stretch that local passes remove only slowly. Making the same passes faster on the GPU gets to the same wrong answer sooner.

So the questions are, in order:

1. Which **local solver** does the per-point work (XPBD as now, or Vertex Block Descent)?
2. What removes the **low-frequency error** that every local solver leaves?
3. How does cloth **collide with itself** without passing through, at this scale?
4. How does it all live on the GPU, behind the engine's seams?

## The headline finding

**No local solver fixes the low-frequency error, by its authors' own account.** This was the most important result of the research, and it corrected our first guess.

- **VBD paper, section 7 [read]:** "A perturbation applied on a vertex can impact other vertices ... at most as far as the number of colors within a single iteration. Therefore, VBD is not ideal for high-resolution stiff systems ... a global solution using Newton's method may prove to be more effective."
- **Augmented VBD, section 6 [read]:** "One fundamental limitation of local iterative techniques like AVBD is information propagation ... propagation can take multiple frames. It would be interesting to explore combining AVBD with a global solver."
- **Periodic Anderson acceleration for VBD (HPG 2026) [read, abstract]:** VBD's "block Gauss-Seidel update propagates information only locally, low-frequency error modes ... decay slowly".

What VBD does improve over XPBD is real but different: a primal formulation (huge mass ratios are fine), a true Newton step per point (more progress per sweep), and fewer colours. The low-frequency stretch needs a **global or hierarchical layer** on top, whichever local solver is chosen.

No published method yet gives 100,000+ inextensible points **with** robust self-collision in real time. The best documented point is Lan et al. 2024: 340,000 points at 4.8 fps on an RTX 3090, penetration-free.

## Part 1: The local solver

### Vertex Block Descent (Chen, Liu, Yang & Yuksel, SIGGRAPH 2024)

[arXiv 2403.06321](https://arxiv.org/abs/2403.06321); code in the authors' Gaia and in NVIDIA Warp, then Newton.

**Implicit Euler as minimisation [read, Eq. 1-3].** One step finds the positions minimising

    G(x) = 1/(2h^2) |x - y|^2_M + E(x),      y = x_t + h v_t + h^2 a_ext

the inertia term pulling each point towards where it would coast, and E the elastic energy. Implicit (backward) Euler is unconditionally stable: it damps rather than blows up.

**The per-point step [read, Eq. 4-9].** Each point i in turn minimises its own share with every other point held still:

    G_i(x) = m_i/(2h^2) |x_i - y_i|^2 + sum over elements j touching i of E_j(x)
    f_i = -(m_i/h^2)(x_i - y_i) - sum_j dE_j/dx_i
    H_i = (m_i/h^2) I + sum_j d^2E_j/dx_i^2           (3 x 3)
    x_i += H_i^-1 f_i                                   (analytic 3 x 3 inverse)

- An element's whole energy enters every point it touches, so each local step lowers the global G (block coordinate descent).
- One iteration is one Newton step per point, colour by colour. Iterations are a fixed count; there is no convergence test.
- **Singular blocks [read]:** if |det H_i| is below a threshold, the point skips that iteration (the paper gives none; Warp 1.6 used 1e-5, Newton 1e-8).
- **Definiteness [read]:** the paper does not force H_i positive definite and says non-definite blocks were rare. **[inf]** Cloth compresses constantly, where StVK's Hessian is indefinite; Newton's choice (below) avoids the problem.
- **Line search [read]:** optional; 40% more cost for no measured gain; unused.

**Vertex colouring [read, section 3.9, Fig. 8].** Points are coloured so that no element holds two points of one colour; then a colour's points move in parallel.

- Far fewer colours than XPBD's constraint colouring: a tetrahedral mesh needed 8 point colours against 76 element colours; the cloth scene 3.
- **Newton's colouring [read, `graph_coloring.py`, Warp `coloring.cpp`]:** maximum cardinality search then ordered greedy (30-50% fewer colours than greedy, twice as slow), then balancing towards a largest-to-smallest group ratio of 1.1.
- **Bending in the colouring [read]:** Newton leaves a hinge's two wing points out of the graph by default (`include_bending=False`), so they may share a colour and race mildly. Its hanging-cloth example turns them in.
- **[inf]** A triangulated grid with hinges takes roughly 5-8 colours, against our 20 constraint colours (6 stretch, 14 bending).

**Chebyshev acceleration [read, section 3.8, Eq. 18-19].** After each full iteration, extrapolate along the last two:

    x(n) = w_n (xbar(n) - x(n-2)) + x(n-2)
    w_1 = 1,   w_2 = 2/(2 - rho^2),   w_n = 4/(4 - rho^2 w_(n-1))

- rho is the estimated spectral radius: 0.95 in most scenes, 0.75-0.93 as stiffness rises.
- A point that collides stops being accelerated for the rest of the step.
- **Not in Newton [read]:** no Chebyshev and no adaptive initial guess in its code. Augmented VBD's author found Chebyshev "kind of unstable and difficult to tune" [read, secondhand via the Houdini port's notes].

**The initial guess [read, section 3.7, Eq. 17].** Starting from y (inertia and gravity, as XPBD does) makes cloth "behave softer than they should" with few iterations. The adaptive guess scales gravity by how much of it the point actually felt last step:

    x = x_t + h v_t + h^2 a~ a_ext,    a~ = clamp(a_t . a_ext_hat / |a_ext|, 0, 1),   a_t = (v_t - v_(t-1))/h

Newton uses plain inertia.

**Damping [read, Eq. 10-11].** Rayleigh: H_i += (k_d/h) sum of element Hessians, f_i -= that times (x_i - x_t). k_d = 1e-5 for the cloth scene. Newton instead damps the rate of the strain metric FᵀF (rigid turns undamped) and the hinge angle's rate.

**Cloth in the paper is thin [read].** One scene: a tearing sheet of 2,500 points on the **CPU**, StVK membrane (mu = lambda = 1e4), h = 1/300 s, 20 iterations, 11.2 ms a step. Only 3 colours, so most likely no bending at all. No GPU cloth timing. The big GPU numbers are tetrahedral solids:

| Scene | Size | Iterations, step | Time (RTX 4090) |
|---|---|---|---|
| Squishy ball | 230K points | 120, 1/120 s | 15 ms a step |
| Octopi | 15.6K points | 10, 1/300 s | 1.1 ms |
| Twisting beams | 97K points | 100, 1/300 s | 60 ms |

- **Against XPBD [read, Fig. 19]:** on the 230K squishy ball, VBD 0.031 s a frame against XPBD's 0.32 s at the same settings, XPBD also needing smaller steps to look alike. A solid, not cloth.
- **Third-party experience [read, MysteryPancake/Houdini-VBD notes; opinion]:** cloth with springs behaves much like Houdini's XPBD (Vellum), with "some weird flattening behaviour"; tetrahedral solids are much more stable.

**GPU layout [read].** The paper: one 16-thread block per point, threads summing the adjacent elements' forces and Hessians through shared memory, about 10 times faster than a thread per point. Newton: 16 lanes per point, a fixed butterfly reduction, two points per 32-lane warp; positions stored as start plus displacement.

### What Newton actually ships for cloth

Newton (`newton/_src/solvers/vbd/`) is where the papers met production. It differs from them on purpose [read]:

- **Membrane: Stable Neo-Hookean, not StVK** (`evaluate_neo_hookean_membrane_force_hessian`, `particle_vbd_kernels.py` 534-664):

        psi = mu/2 (I_C - 2) + lambda'/2 (J - alpha)^2
        I_C = tr(FᵀF),   J = sqrt(det(FᵀF)),   lambda' = lambda + mu,   alpha = 1 + mu/lambda'

  F is the 3 x 2 deformation gradient [x1 - x0, x2 - x0] Dm^-1, with Dm^-1 the rest triangle's 2 x 2 inverse in its own plane. Its 3 x 3 blocks are made positive definite analytically (the formula is in the code; **[check]** before writing tests). Energy is psi times the rest area.
- **Units [inf, from the code's scaling]:** with energy per area, mu and lambda are surface moduli in N/m (Young's modulus times thickness), not the Pa the docs say. Isotropic plane stress gives mu = E t / (2(1 + nu)) and lambda = E t nu / (1 - nu^2) **[check]**. Our cotton's tensile stiffness of about 2000 N/m sits there.
- **Bending: the dihedral angle, Gauss-Newton** (`evaluate_dihedral_angle_based_bending_force_hessian`, 696-848): E = 1/2 k L0 (theta - theta0)^2, Hessian k L0 grad(theta) grad(theta)ᵀ only, always positive semi-definite. Scaled by the rest edge length only, so its stiffness changes with mesh resolution **[inf]**. Ours (B 3 l^2 / A, discrete shells) does not, and should be kept.
- **No Chebyshev, no adaptive guess, no line search.** Default 10 iterations; examples 4-10 iterations by 10 substeps at 60 fps.
- **Units for float32 [read]:** its Franka T-shirt example runs in centimetres "for better numerical behaviour".
- **Deterministic mode [read]:** float atomics rewritten into records, a stable sort and fixed-order reductions; elasticity is deterministic by construction (a gather per point over a fixed adjacency order).
- **Self-contact is off by default** and "usually needs additional tuning" (Isaac Lab's docs). The solver is marked experimental, its API changing fast.
- **No published timings** in the repository or Isaac Lab.

### Augmented VBD (Giles, Diaz & Yuksel, SIGGRAPH 2025)

[Project page](https://graphics.cs.utah.edu/research/projects/avbd/). It fixes VBD's other weakness, **stiffness ratios**, with an augmented Lagrangian.

**Hard constraints [read, Eq. 8-12].** An energy 1/2 k C^2 + lambda C per constraint, with after each sweep:

    lambda <- k C + lambda          (clamped to bounds for inequalities)
    k      <- k + beta |C|          (the penalty ramps with the violation)

- Finite-stiffness energies ramp k from small up to their real stiffness (Eq. 16), so a point sees a mild stiffness ratio in early iterations.
- **Hessian [read, Eq. 17]:** k JᵀJ plus the constraint curvature's indefinite part lumped to a diagonal of its column norms, solved with LDLᵀ: always positive definite.
- **Parameters [read, Table 2]:** beta 10 (the demos 1e4-1e5: "will depend on ... the units"), alpha 0.95 (stabilisation against explosive error correction), gamma 0.99 (warm start of k and lambda across steps).
- **Results [read]:** springs 1:10,000 converge in 5 iterations, 1 with warm start, where VBD still sags at 100; a 1,500-point mass-spring flag at 5 iterations beats VBD at 20. 110,000 rigid blocks in 3.5 ms (RTX 4090, DirectX 11 compute).

**For cloth it is unproven [read].** The paper's cloth is a mass-spring flag and a 10,000-point sheet under 35,000 rigid bodies (16 ms a frame with collision detection). Newton uses AVBD **for rigid bodies only**; its cloth stays plain VBD, and Newton 1.6 already replaced the paper's rigid form with "compliant ALM" (effectively XPBD's compliance inside a primal solver). The Houdini port's author: AVBD about twice as slow as VBD, stiff constraints still exploding, XPBD "better for stiffness" (opinion).

**[inf]** Its value for us is the idea, not the package: stretch edges as hard or compliant augmented-Lagrangian constraints carrying their tension in lambda, so the penalty in the 3 x 3 block stays moderate. That is what XPBD already does with its multipliers.

### What it means here

- **XPBD is not the problem; locality is.** XPBD and VBD share the propagation limit. VBD's gains (mass ratios, fewer colours, a Newton step, contact as energy) are real but secondary for a sheet of equal masses.
- **VBD earns its place through self-collision.** The penetration-free contact methods below (OGC, DAT) were built inside VBD and assume an energy-based, primal solver. With XPBD they would need adapting.
- **The decision should be measured, not read.** Both solvers on the same GPU framework, judged by the same tests (Part 5), on our sheets at our units. Neither paper measured large cloth against the other.
- **Keep our bending energy** (B 3 l^2 / A, isometric or hinge) in either solver: its stiffness does not change with the mesh, which Newton's does.

## Part 2: The low-frequency layer

Every local solver needs one of these on top for large, fine garments.

### Hierarchical position-based dynamics (Müller 2008)

[Paper](https://matthias-research.github.io/pages/publications/hpbd.pdf), read in full on Oct 25 2026. A nonlinear multigrid: PBD's own constraint projection run on ever coarser copies of the mesh, coarsest first.

**The hierarchy [read].**

- Level 0 is the mesh. Each coarser level l+1 keeps a subset of level l's points. A point's *level* is the coarsest level that holds it.
- Two points are neighbours on a level when a constraint joins them there. Every point whose level is l must have at least k neighbours on level l+1: its *parents*. The paper uses k = 2.
- Constraints on one point (contacts, attachments) go with the point to every level it is on.
- Only stretch is carried to coarser levels. Bending stays on level 0: "increased bending stiffness typically removes small detail and wrinkles".

**Choosing the coarse points [read].** One greedy sweep, for any mesh, disconnected pieces too:

1. Mark every point coarse, and give each a count of its coarse neighbours (at first, all its neighbours).
2. Visit the points in any order. Mark a point fine if its count is at least k and every fine neighbour's count is above k. Then lower each neighbour's count by one.
3. Attached (pinned) points help convergence when kept coarse, but keeping all of them crowds the coarse levels. Visit them last, or let them go fine only with some probability.

A bigger k shrinks the levels more slowly: smoother propagation, more work on the coarse levels.

**Parents' weights [read].** With d_ij the rest distance from point i to its parent j:

| | |
|---|---|
| Raw weight | ŵ_ij = 1 / (d_ij / max_j d_ij + ε) |
| Weight | w_ij = ŵ_ij / Σ_j ŵ_ij |

A point lying on a parent follows that parent alone; ε keeps the weight finite. The paper does not give ε's value.

**Coarse constraints, by collapsing edges [read].** Level l+1 starts as a copy of level l's constraints. Each fine point i is then collapsed, one by one, into one of its coarse neighbours j: the one nearest the average of its coarse neighbours' rest positions, which keeps edge lengths even. The edge i-j goes. Each other edge i-k becomes k-j, or goes if k-j already exists. A triangle mesh stays a triangle mesh.

The new edge's rest length is one of two:

- **The sum of the two rest lengths** (k-i plus i-j): a path along the mesh. Never too tight and never bridges a cut, but too loose, since paths along edges are longer than geodesics.
- **The straight distance in the rest pose:** exact for a flat sheet, too tight for a curved one, and it bridges cuts.

Every coarse constraint is a **one-sided upper limit**, C = d - |p_1 - p_2| >= 0, skipped while the cloth between is slack. So the hierarchy never resists folding or bending, only over-stretch.

**The solve [read].** Once per step, before the plain solver:

1. Start at the coarsest level.
2. Save the positions q_i of this level's points.
3. Run one or more PBD passes over this level: its points' one-point constraints and its own stretch limits.
4. If this is level 0, stop. Otherwise go one level finer.
5. Move each point of the new level by its parents' corrections: p_i <- p_i + Σ_j w_ij (p_j - q_j), over its parents j.
6. Go to 2.

Only corrections go down, never positions, so the fine level keeps its folds and wrinkles. The paper then runs the plain solver on level 0 twice, the second pass in reverse order (symmetric Gauss-Seidel).

**Results [read].**

- A sheet 126 rows of triangles tall (Fig. 3): plain PBD needs 5 iterations to hold stretch to 25% and 30 for 5%. With 5 levels, 1 iteration holds 9% and 5 iterations 5%. With 3 levels, 20 iterations for 5%.
- An 11,500-triangle sheet with the hierarchy and 2 iterations runs at 60 fps. Plain PBD matches its stiffness only at 20 iterations, at 12 fps. The hierarchy adds little cost: all its levels together hold fewer constraints than level 0.
- An irregular dress, coarsened greedily, at 30 fps. On regular sheets the paper skips the greedy sweep and uses coarser grids.
- **Limits:** poor coarse meshes show artefacts at very few iterations (gone with 3-4 fine passes). Tearing must cut parent links all the way up. The paper ran the hierarchy serially, before a parallel PBD. MGPBD's authors say such methods "may struggle to ensure physical consistency between levels".

**Where the text is loose [read, inf].**

- **Which points step 5 moves.** "Each particle on level l" must mean the points whose level is l. Points also on level l+1 were moved by that level's own solve, and parents are defined only for the others.
- **What q holds with three or more levels.** Read literally, q is saved again on every level, after step 5's move. So a parent's p_j - q_j holds only the last level's own solve: the pull a level 2 solve gave a level 1 point never reaches its children on level 0. That pull is then left for the fine solve, the very low-frequency error the hierarchy is meant to carry. Saving every point's q once, before the coarsest level, and passing down each parent's whole motion carries every level's correction. With two levels both readings agree. **We build the second**, and can try the literal one on the yardstick if the numbers disappoint.
- **Level 0.** The steps solve level 0 last, while the results say the plain solver runs afterwards because the hierarchy "does not consider the original constraints on level 0". In effect the same: the hierarchy's levels from the coarsest to level 1, its corrections passed down to level 0, then our usual substep.
- **Stiffness and masses.** Coarse points keep their own masses. In XPBD terms a coarse limit is hard (compliance 0), so it needs no multiplier kept between passes.

**What it means here [inf].**

- **Rest lengths: the straight distance in the rest pose.** Every cloth today is a flat `Mesh::grid`, its diagonals all one way. Paths along its edges are far too long: across the diagonals, two edges for √2 of distance, 41% slack, where fabric should stretch about 0.15%. A path-sum limit would never grip there. For garments (phase 10), sewn panels are flat in pattern space, so a panel's pattern distance stays exact; path sums only where a coarse edge crosses a seam. A mesh with a curved rest shape would get limits too tight, but no cloth has one.
- **Deterministic:** the greedy sweep visits points in index order, pins last, so a mesh always gets the same hierarchy.
- **Where it runs:** once a substep, after the prediction and after the pins and the hand have moved, before the stretch and bending colours. The velocity comes from the motion as now, so nothing else changes.
- **One-point constraints on coarse levels:** pins (inverse mass 0, so they pull hardest) and the hand. Contacts with bodies are open: without them a coarse pass may pull points into the ball for the fine contacts to push out; with them, more cost. Start with pins and the hand, and judge on the draped sheet.
- **On the GPU (the next branch):** each level is a set of limits coloured as today (`colourStretch` colours any edge list), plus one dispatch a level passing its corrections down.
- **Judged on the yardstick:** path stretch near 0.15% and the same sag on every rung, and its cost a tick.

**Built and measured** (`hierarchical-cloth`, Oct 25 2026; sheets settled 4 s in the tests' debug build, path stretch; the yardstick's own rows are in Part 5). Two of the paper's choices changed:

- **Three parents, not two.** With the paper's k = 2 each level is about 3 times smaller than the one below, and many children follow only two parents. It helped (a 128-square sheet from 14.7% to 2%), but hierarchies of 7 or more levels shook a settled sheet apart. With k = 3 (levels about 2.2 times smaller): 32 squares 0.035%, 64 0.11%, 128 0.32%, 200 0.32%, against 1.5%, 5.6%, 14.7% and 33.5% without. k = 4 was mixed (0.19% at 128 squares with 10 levels, but 0.054% against 0.025% at 64) and coarsens more slowly (about 1.8 times a level), so it needs more levels and passes for the same reach.
- **A smallest level.** Every hierarchy that shook a sheet apart ended in a level of about 30 points or fewer (k = 2 or 3 or 4 alike), each edge spanning a fifth of the sheet; none ending at 50 or more did. Levels stop at 50 points (`smallestClothLevel`): a 200-square sheet gets 9.
- **The whole motion passed down, confirmed:** the paper's literal re-save on every level, at k = 3, left 0.11%, 0.52%, 1.5% and 1.9%, about 5 times as much. Passing nothing down (only the coarse points corrected) left more still.
- **Tried and dropped:** weights that also pass on a stretch or turn of the parents (least squares, kept near the paper's) shook every deep hierarchy apart: they go negative where a child lies outside its parents, and a child with two parents off its line cannot be placed by them at all. The fabric's compliance on the coarse limits instead of none changed almost nothing (the points' inverse masses dominate a substep's step); kept, so each level is the same fabric.
- **Not a one-pass fix:** a sheet stretched 10% all the way down comes back only to 8.6% in one pass (9.8% without), since a parents' average passes on a shift exactly but a stretch only roughly. Each substep's correction is small (gravity moves a point 3 microns), which the layer carries well.
- **The worst edge stays high** near the corner pins (17% at 200 squares), where one point holds the sheet: local, not the sheet-wide error the layer is for.

### Tethers and strain limits

- **Long-range attachments (Kim, Chentanez & Müller 2012) [read, abstract]:** a maximum-distance constraint from each point to its nearest attachment along the cloth. Global inextensibility for O(1) cost, but only for attached cloth (a worn garment's shoulders); free cloth gets nothing. Unreal's Chaos tethers can cause "pull string puppet like behavior" [read].
- **NvCloth [read, docs]:** tethers cooked offline with a slack scale (1.2 = 20%), constraints solved in phases, 300 Hz by default.
- **Strain limiting [read, C-IPC, StiffGIPC]:** clamping each edge to at most 1.0-1.1 times its rest length after the solve. Local again, so on its own it has the same reach problem.

**What it means here [inf].** Tethers are a cheap option for worn garments in phase 10 (shoulders, waistband), not a solver.

### Global solvers

- **MGPBD (2025) [read]:** XPBD's full system solved by conjugate gradients with algebraic multigrid. Cloth up to 263,000 points stays inextensible where Jacobi XPBD stalls, but **no cloth timings** and offline speeds elsewhere (seconds a frame). Needs explicit sparse matrices, collision only against a static distance field. Too heavy for us now.
- **Projective dynamics with subspace reuse (Lan et al. 2024, Style3D) [read]:** Jacobi for the high frequencies, a precomputed low-mode subspace of the rest shape for the low ones, penetration-free barriers. 340,000 points at 4.8 fps, a drape of 120,000 degrees of freedom at 25 fps (RTX 3090). Precomputation 6-47 s per garment, quadratic materials only. [arXiv 2403.19272](https://arxiv.org/abs/2403.19272)
- **Chebyshev (Wang 2015; Wang & Yang 2016) [read]:** a constant-factor speed-up on top of any local solver, best with Jacobi on the GPU, weaker for PBD-style projection; rho tuned by hand. A 15,000-point dress at 26.6 ms (GTX TITAN X, 8 substeps of 40 iterations).

**What it means here [inf].** The subspace is a strong later option for the dressmaking game, where a garment's rest shape is fixed between pattern edits (rebuild in seconds after an edit). The hierarchy comes first: simpler, no precomputed solve, no material limits.

## Part 3: Self-collision

The roadmap's current plan (a broad phase, continuous collision detection on vertex-triangle and edge-edge pairs, then an untangling pass) is what these methods replace.

### Offset Geometric Contact (Chen et al., SIGGRAPH 2025)

[Project page](https://graphics.cs.utah.edu/research/projects/ogc/), [PDF](https://graphics.cs.utah.edu/research/projects/ogc/Offset_Geometric_Contact-SIGGRAPH2025.pdf). Cloth as a surface offset by a radius r to both sides, with contact forces always perpendicular to the feature they push from.

**Why not IPC's contact [read, section 2.2].** IPC inflates every face into a capsule; near edges and corners its forces are not perpendicular to the cloth, stretching it in its own plane and pushing a point's own neighbours apart. With a 5 mm radius that bulges and oscillates, so IPC shrinks the radius, which stiffens everything.

**The blocks [read, Eq. 6-12].** Each feature's offset region, cut so that neighbouring regions do not overlap:

- **face:** a prism of half-height r on both sides, bounded by planes through its edges;
- **edge:** a cylinder of radius r cut by the planes towards its two neighbouring faces' far corners (a half-cylinder on a border);
- **vertex:** a ball of radius r cut by planes perpendicular to its edges, empty at saddle points (some neighbour always covers them).

A point's contact set is the features (not its own) whose block holds it; their count is how many layers it is inside. Edge-edge contacts use the same construction on the wireframe.

**The contact energy [read, Eq. 18-20].** Two stages, with d the distance and tau = r/2:

    g(d) = k_c/2 (r - d)^2                 for tau <= d <= r
    g(d) = -k_c' log(d) + b                for 0 < d < tau
    k_c' = tau^2 k_c,   b = k_c/2 (r - tau)^2 + k_c' log(tau)

- The paper's Eq. 19 prints k_c' = tau k_c (tau - r)^2; matching slopes needs tau k_c (r - tau), which is tau^2 k_c at tau = r/2. Newton's code has tau^2 k_c [read].
- Newton continues below d_min = min(1e-5, tau/2) with a quadratic, so the barrier never divides by zero [read].
- **Per point [read, Newton]:** f = -g'(d) beta n, H = g''(d) beta^2 n nᵀ, beta the point's barycentric weight in the pair. The Hessian drops the normal's own derivative: a Gauss-Newton block, always positive semi-definite.
- k_c: 1e5 for most cloth, 1e4 on a sphere; units not stated [read].

**The penetration-free guarantee [read, section 3.7, Eq. 21-27].** Each point gets a bound on how far it may move:

    b_v = gamma_p min(distance to the nearest face not its own,
                      its edges' distances to the nearest other edges,
                      its faces' distances to the nearest other points),     gamma_p = 0.45

If every point stays within its bound, no pair can close by more than 2 gamma_p < 1 of its gap, so nothing passes through, with **no continuous collision detection**.

- After each iteration, a point past its bound is pulled back along its motion to the bound.
- Collision detection reruns once a share gamma_e = 0.01 of the points have hit their bounds.
- Distances are searched within a query radius r_q >= r, which also caps the bounds; the paper suggests r plus the step's inertial motion.
- **Must start intersection-free**; seams need their stitched features filtered out.

**Collision detection [read].** Two bounding volume hierarchies (faces, edges) over the original features. A thread per point queries faces within r_q; a thread per edge queries edges. About 10% of the run time with VBD.

**Results [read, Table 1, RTX 4090, per step]:**

| Scene | Points | Contact radius | Step, iterations | GPU time (average / max) |
|---|---|---|---|---|
| 50 layers on a cylinder | 246K | 2-3 mm | 1/1200 s, 40 | 6.3 / 11.5 ms |
| Knot | 48K | 2 mm | 1/300 s, 50 | 4.4 / 6.8 ms |
| Twisted cloth, 8 turns | 10K | 2 mm | 1/300 s, 10 | 0.9 / 1.5 ms |
| Dress on a moving body | 15.7K | 2 mm | 1/200 s, 20 | 1.2 / 1.4 ms |
| Robot folding a T-shirt | 13.8K | 2 mm | 1/600 s, 10 | 1.8 / 2.2 ms |

- Every frame was checked for intersections; none were found.
- Against C-IPC's dress: 24 s a frame against 9.6 ms on the GPU.
- Body against cloth is plain penalty contact: **cloth can sink into a moving body; only cloth against cloth is guaranteed.**
- **Limits [read, section 6]:** a force jump where a point enters a block through its open concave side; points falling side by side are slowed to 2 gamma_p of their gap per detection (fixed by DAT below).
- **No public code.** Several inconsistencies between the table and captions (the 50 layers' point count and radius).

### Divide and Truncate (Chen, Hsu, Ayman & Macklin, 2026)

[arXiv 2604.15513](https://arxiv.org/abs/2604.15513) (not yet read in full; this is from Newton's code). Newton does **not** implement OGC's blocks yet ("Coverage of Offset Geometric Contact will be added once it is available in Newton"); it ships OGC's two-stage energy on plain closest-point contacts, with this guarantee instead [read, `rigid_vbd_kernels.py` 7543-7925, `particle_vbd_kernels.py` 2053-2211]:

- For each contact pair, find a certified separating plane (the closest direction first, then face normals and edge axes).
- Place it between the two in proportion to how fast each side approaches, keeping at least 5% of the gap on each side.
- Each point's straight motion is cut at gamma = 0.85 of its crossing with each of its pairs' planes; the smallest cut wins (an atomic minimum), plus a cap of 0.5 gamma r_q on any motion.
- Only motion **towards** a neighbour is cut, so OGC's slowed falling pairs go away.
- Runs after **every colour** of every iteration. A pair already intersecting has no separator and is left to the contact forces.
- Detection on a fixed schedule (before and after the initial guess by default), the hierarchies refitted, rebuilt only when asked. Fixed buffers of 32 vertex-face and 64 edge-edge pairs per feature; overflow is **silently dropped**.

### What it means here

- **The contact energy and a motion bound replace the whole old plan.** No continuous collision detection, no untangling pass: the guarantee holds by construction as long as the cloth starts clean.
- **DAT is the more practical bound** (direction-aware, already in production); OGC's blocks give the cleaner forces. Read the DAT paper before choosing **[check]**.
- **Both assume an energy-based solver** whose points take bounded steps, which VBD is. In XPBD, contacts would be inequality constraints with the same truncation after each colour **[inf]**; untested in the literature.
- **Cloth against bodies stays penalty contact** (our current shapes, later the mannequin's distance field); a moving body can push cloth into itself.
- **Real time needs small steps:** 1/200-1/1200 s with 10-50 iterations in their scenes. Our 15 substeps a tick are 1/1800 s.

## Part 4: Fitting the GPU and the engine

### The design: the cloth lives on the GPU

- **The renderer owns the cloth's buffers** (positions, rest data, colours, adjacency, contacts) and records each frame's ticks of cloth passes into its command buffer. Nothing is copied up and down per tick except the game's commands (pins, hands, colliders).
- **Drawn straight from the cloth's buffer:** the deforming mesh's normals and tangents computed in a compute pass, no CPU upload. Interpolation between ticks from the last two ticks' positions, as the CPU path does.
- **Positions come back a frame or two late**, read back when that frame in flight comes round (as the probe relocation records already do), for picking, grabbing and gameplay queries.
- **The seam:** games never include `render/`. An interface in `cloth/` (a GPU cloth service) is handed to games the way the job system is (`Game::getJobs`); headless runs and tests keep the CPU solver, which becomes the reference mirror.
- **Determinism:** GPU cloth is bit-for-bit repeatable on one GPU and driver only, like the golden images, so it leaves `hashSimulationState`; the CPU path keeps its tests.

### Compute shader details from the research

- **One small group per point**, summing its elements' forces and 3 x 3 blocks through shared memory in a fixed tree order: deterministic and, per the VBD paper, about 10 times faster than a thread per point. Subgroup operations (`subgroupAdd`) are faster but their order is the driver's **[inf]**.
- **Adjacency in compressed rows:** per point, its triangles and hinges with its own corner index, so each group computes only its own block.
- **No float atomics for sums:** each point gathers its own contacts from lists sorted in a fixed order. A minimum (DAT's cut) is order-free; GLSL has no core float atomicMin, but non-negative floats compare like their bits, so an integer atomicMin on the bits works **[inf]**.
- **Dispatch count:** a colour per dispatch with a barrier between. With about 6 point colours, a few hierarchy levels and contact passes, about 15-25 dispatches a substep **[inf]**.
- **Precision:** single-precision error grows with many small steps (Macklin 2019 [read]); Newton runs a garment in centimetres for that reason. Positions relative to the cloth's own centre help **[inf]**.

### Scale and cost

- **RTX 5080 [inf]:** roughly an RTX 4090's class or a little below in compute, so the 4090 timings above are a fair guide.
- **Typical GPUs:** the cloth's quality (substeps, iterations, hierarchy levels) must be a setting with presets, like the lighting's ([memory: scalable graphics]).
- **Budget [inf, from OGC's dress]:** a 15,000-point garment with self-collision at about 1.2 ms a step and 4-8 steps a frame is 5-10 ms: real time for one hero garment, as the dressmaking game needs. A 250,000-point pile is far from it.

## Part 5: How we judge it

Before any solver is chosen, tests that measure the problem, on the CPU and the GPU alike:

- **The resolution ladder:** the same 1 m sheet hung from two corners at 25, 50, 100 and 200 squares a side, the same fabric. Measure its stretch (longest path against rest) and its sag once settled. A good solver gives the same answer at every resolution; ours today does not.
- **The cantilever** (`testCantilever`), already there: droop against resolution and substeps.
- **Cost per tick** for each rung, on one GPU, as the cloth lab's measurements are now.
- **Self-collision:** a sheet dropped onto itself into a pile, checked for intersections every frame (phase 7's second milestone), and a twisted sheet.
- **GPU against CPU:** the shader library's functions against their C++ mirrors (`make gputest`), as for every other shader library.

**Measured** (`make yardstick`, release, 16 threads, Oct 25 2026: today's CPU solver, XPBD with isometric bending, the default cotton, settled 10 s):

| Squares | Points | Substeps | Path stretch | Worst edge | Sag (m) | Moved in the last second (mm) | ms a tick |
|---|---|---|---|---|---|---|---|
| 25 | 676 | 15 | 0.97% | 5.8% | 1.007 | 0.11 | 0.42 |
| 25 | 676 | 60 | 0.31% | 1.5% | 1.002 | 0.13 | 1.64 |
| 50 | 2,601 | 15 | 3.2% | 43% | 1.023 | 0.31 | 1.65 |
| 50 | 2,601 | 60 | 0.39% | 3.6% | 1.003 | 4.35 | 6.58 |
| 100 | 10,201 | 15 | 9.7% | 210% | 1.074 | 13.9 | 4.90 |
| 100 | 10,201 | 60 | 0.87% | 17% | 1.007 | 2.67 | 19.8 |
| 200 | 40,401 | 15 | 36% | 872% | 1.311 | 14.4 | 16.2 |
| 200 | 40,401 | 60 | 4.7% | 130% | 1.027 | 55.0 | 69.4 |

- **The low-frequency error, measured:** the fabric should stretch about 0.15%. At 15 substeps the path stretch roughly triples with each halving of the squares, 1% to 36%, and the 200 sheet hangs 31 cm too low. Four times the substeps (and four times the cost) cuts it about eightfold but does not stop the growth.
- **The worst edge is not all the solver's:** each pin is a single point, and a point holding a sheet has a stress that grows without bound near it even in the exact solution, so the edges at the pins stretch more on a finer mesh whatever the solver. Path stretch and sag are the yardstick's main numbers; the worst edge shows where the error sits.
- **Not quite settled** at 60 substeps on the finer sheets (a few cm a second on the 200): the stiffer sheet still sways a little after 10 s, under 0.5 /s drag.
- **The goal for the later steps:** the same path stretch and sag on every rung, near 0.15% and 1.00 m, at no more than 60 substeps' cost.

**The same solver on the GPU** (`gpu-cloth-solver`, `make yardstick`'s GPU rows, Oct 25 2026, RTX 5080, ms a tick counting recording, submitting and waiting, 120 ticks a submission):

| Squares | Points | CPU, 15 substeps | GPU, 15 | CPU, 60 | GPU, 60 |
|---|---|---|---|---|---|
| 25 | 676 | 0.41 | 0.75 | 1.61 | 2.94 |
| 50 | 2,601 | 1.63 | 0.76 | 6.46 | 3.13 |
| 100 | 10,201 | 5.13 | 0.79 | 20.3 | 3.10 |
| 200 | 40,401 | 14.8 | 0.89 | 59.3 | 3.64 |

- **The same answers:** stretch, worst edge and sag match the CPU's rows to the shown digits, except the 200 sheet at 60 substeps, still swaying (2.6 to 5.5 cm in its last second), where a sheet in motion is chaotic.
- **Cost is the dispatches, not the points:** 330 dispatches a tick at 15 substeps (22 a substep: the prediction, about 20 colours, the end), about 2.3 microseconds each whatever their size, so 40,000 points cost barely more than 700. The 200 sheet is 17 times faster than the CPU's 16 threads; the smallest is slower than the CPU.
- **What it means for the plan:** the GPU has room for many more substeps or a hierarchy's extra passes on large garments; fewer dispatches (fewer colours, fused passes, a whole small cloth in one workgroup) would help small cloths.

**The low-frequency layer on the CPU** (`hierarchical-cloth`, Oct 25 2026, release, 16 threads; every level each sheet has, three parents a point, none below 50 points; Part 2):

| Squares | Levels | Path stretch, 15 substeps | Sag (m) | ms a tick | Path stretch, 60 | ms a tick |
|---|---|---|---|---|---|---|
| 25 | 3 | 0.054% (0.97% without) | 1.0004 | 0.63 (0.43) | 0.017% | 2.61 |
| 50 | 5 | 0.086% (3.2%) | 1.0006 | 3.14 (1.77) | 0.011% | 13.3 |
| 100 | 7 | 0.154% (9.7%) | 1.0009 | 13.2 (6.06) | 0.014% | 53.3 |
| 200 | 9 | 0.312% (36%) | 1.0021 | 30.9 (19.8) | 0.030% | 122 |

- **The goal nearly met:** at 15 substeps the path stretch is 0.05-0.3% on every rung, near the fabric's 0.15%, and every sheet hangs within 2 mm of its metre, where the 200 sheet hung 31 cm too low. It still grows a little with the squares (1.6 to 2 times a rung, against about 3 without).
- **Cheaper than substeps:** 15 substeps with the levels beat 60 without on every rung, for less: the 200 sheet 0.31% in 31 ms against 4.7% in 80.
- **Stiffer than the fabric** where it is cheap enough to run long: at 60 substeps 0.01-0.03%, under cotton's 0.15%. The coarse limits are one-sided at the rest length, so once they converge the sheet barely stretches at all; invisible at these sizes.
- **The cost on the CPU** is 1.5-2.2 times the tick: each level's limits come in a few colours of few constraints, each colour a barrier (the same limit as the fine solve's). On the GPU each level is a few more dispatches.
- **The worst edge** falls too (872% to 17% at 200 squares) but stays the largest near the corner pins.
- **Times in this run** for the plain CPU rows were about 20% slower than in `cloth-yardstick`'s (19.8 ms against 16.2 at 200 squares), and the GPU rows varied between 0.9 and 1.7 ms at 15 substeps: the machine's load, not the code.

## The plan

Each step is a branch of its own, explained first, tests first.

1. **The yardstick (done, `cloth-yardstick`).** The resolution ladder on today's CPU solver, measured and recorded (Part 5). Small, and it tells every later step whether it helped.
2. **The GPU cloth framework.** The GPU-resident design (Part 4) with today's XPBD solver ported as it is (coloured, our units), its GLSL library mirrored by the CPU solver and checked by GPU tests. The big sheet on the GPU, drawn from its buffer, picked and grabbed through the late readback. Measured on the ladder.
3. **The low-frequency layer (CPU done, `hierarchical-cloth`; GPU next).** Hierarchical levels with one-sided distance limits (Müller 2008; rest lengths straight across the flat rest shape, Part 2), CPU and GPU, judged on the ladder. Expected to be the biggest visible gain for large garments.
4. **Vertex Block Descent as a second local solver,** on the same framework: Stable Neo-Hookean membrane in N/m from our fabric's stiffness, our bending energy, point colouring, the adaptive guess, Chebyshev to try. Judged against XPBD on the ladder and cantilever. Keep the better, or both if each wins somewhere.
5. **Self-collision.** The two-stage contact energy with DAT's motion bound (or OGC's blocks, after reading DAT), a bounding volume hierarchy on the GPU, the pile milestone. Built for whichever solver step 4 keeps.
6. **Bodies on the GPU:** the engine's collider shapes as now, later the mannequin's distance field (already a roadmap item).

Later, for the dressmaking game: tethers for worn garments, the coarse simulation mesh driving a fine render mesh (already on phase 10's list), and a precomputed subspace per garment if the hierarchy is not enough.

## Where it goes in the roadmap

Agreed with the user on Oct 25, 2026; phase 7's items in `ROADMAP.md` changed to match:

- "A faster barrier for the coloured solve ... or the GPU" became steps 1-2 (the CPU barrier stays an option for the CPU path).
- Steps 3 and 4 are new items.
- "Self-collision" and "Untangling pass" became step 5, the untangling pass dropped (the motion bound makes it unnecessary for cloth that starts clean).
- "Collision with a mannequin" stays as it is (step 6).
- "Optional, later: IPC" stays, as the offline reference.

## Sources

- Chen, Liu, Yang & Yuksel 2024, "Vertex Block Descent", ACM TOG 43(4). [arXiv 2403.06321](https://arxiv.org/abs/2403.06321), [project](https://graphics.cs.utah.edu/research/projects/vbd/)
- Giles, Diaz & Yuksel 2025, "Augmented Vertex Block Descent", ACM TOG 44(4) 90. [Project](https://graphics.cs.utah.edu/research/projects/avbd/); demos `savant117/avbd-demo2d`, `avbd-demo3d`
- Chen et al. 2025, "Offset Geometric Contact", ACM TOG 44(4). [Project](https://graphics.cs.utah.edu/research/projects/ogc/)
- Chen, Hsu, Ayman & Macklin 2026, "Divide and Truncate". [arXiv 2604.15513](https://arxiv.org/abs/2604.15513)
- NVIDIA Newton, `newton/_src/solvers/vbd/` (main at `f121272e`). [GitHub](https://github.com/newton-physics/newton); Isaac Lab's [VBD tuning page](https://isaac-sim.github.io/IsaacLab/main/source/how-to/solver_tuning_vbd.html)
- Müller 2008, "Hierarchical Position Based Dynamics". [PDF](https://matthias-research.github.io/pages/publications/hpbd.pdf)
- Macklin et al. 2019, "Small Steps in Physics Simulation". [PDF](https://mmacklin.com/smallsteps.pdf)
- Macklin et al. 2014, "Unified Particle Physics for Real-Time Applications" (Flex). [PDF](https://mmacklin.com/uppfrta_preprint.pdf)
- Kim, Chentanez & Müller 2012, "Long Range Attachments". [Eurographics](https://diglib.eg.org/handle/10.2312/SCA.SCA12.305-310)
- Wang 2015, "A Chebyshev Semi-Iterative Approach". [PDF](https://wanghmin.github.io/publication/wang-2015-csi/Wang-2015-CSI.pdf); Wang & Yang 2016, "Descent Methods for Elastic Body Simulation on the GPU". [PDF](https://wanghmin.github.io/publication/wang-2016-dme/Wang-2016-DME.pdf)
- Wang 2021, "GPU-Based Simulation of Cloth Wrinkles at Submillimeter Levels". [PDF](https://wanghmin.github.io/publication/wang-2021-gbs/Wang-2021-GBS.pdf)
- Lan et al. 2024, "Efficient GPU Cloth Simulation with Non-distance Barriers and Subspace Reuse". [arXiv 2403.19272](https://arxiv.org/abs/2403.19272)
- Li et al. 2025, "MGPBD: A Multigrid Accelerated Global XPBD Solver". [arXiv 2505.13390](https://arxiv.org/abs/2505.13390)
- Huang et al., "StiffGIPC". [arXiv 2411.06224](https://arxiv.org/abs/2411.06224); Li et al. 2021, "Codimensional Incremental Potential Contact". [arXiv 2012.04457](https://arxiv.org/abs/2012.04457)
- JGS2, SIGGRAPH 2025. [arXiv 2506.06494](https://arxiv.org/abs/2506.06494)
- MysteryPancake, Houdini-VBD (third-party port and its authors' answers). [GitHub](https://github.com/MysteryPancake/Houdini-VBD)
