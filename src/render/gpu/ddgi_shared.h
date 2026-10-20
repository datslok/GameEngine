#ifndef RENDER_GPU_DDGI_SHARED_H
#define RENDER_GPU_DDGI_SHARED_H

// Global illumination's light probes (DDGI; render/gpu/ddgi.h, ddgi.glsl and the passes that include it). Only
// #defines of plain numbers, since GLSL includes this file too; C++ reads them as typed constants in render/gpu/ddgi.h.

// Texels per side of a probe's two octahedral images, without their one-texel border: the light arriving (irradiance)
// and the distances to the nearest surfaces. Both papers' sizes; an 8 x 8 probe is one 64-invocation workgroup.
#define DDGI_IRRADIANCE_TEXELS 8
#define DDGI_DISTANCE_TEXELS 16

// Rays each probe may trace per frame (the papers measured 32 to 256). The update holds a probe's rays in shared memory,
// so the largest count sizes that.
#define DDGI_MIN_RAYS 16
#define DDGI_MAX_RAYS 256

// Frames that take turns through one spiral of rays: frame f of a cycle traces every DDGI_RAY_CYCLE-th point of a
// spiral DDGI_RAY_CYCLE times as long, from point f, so each cycle covers the sphere evenly between them. Measured in the
// light lab's back room (lit by a small sunlit patch through a doorway), 16 left the least flicker: about a third of
// independent random rotations each frame, as good as three times the rays; 8 and 64 were slightly worse. Fast lights'
// short memory (0.05 s, a few frames) flickered least on the same cycle too: shorter cycles under its rotation trace the
// same directions again and again until it turns (render/gpu/ddgi.h, GlobalIlluminationSettings::fastRaysPerProbe).
#define DDGI_RAY_CYCLE 16

// How sharply a distance texel favours rays near its own direction (a cosine raised to this power), so distances stay
// crisp at the edges of walls [2019].
#define DDGI_DISTANCE_SHARPNESS 50.0

// A ray that met a back face records its distance as this share of the real one: the probe treats whatever lies behind
// as close and hidden, so nothing is lit through it [2021].
#define DDGI_BACK_FACE_DISTANCE_SHARE 0.2

// A texel whose rays' summed weight is below this keeps its old value (no ray came near its direction).
#define DDGI_MIN_TEXEL_WEIGHT 1e-6

// The longest distance a distance texel records, in cell diagonals ([2019]: 1.5): far enough to see past a neighbour.
#define DDGI_MAX_DISTANCE_DIAGONALS 1.5

// How far a probe ray looks for a surface, in metres: far enough for any level, so open sky is only where nothing is.
#define DDGI_RAY_REACH 100000.0

// Shadow rays from a probe ray's hit start this far out along the surface's normal, in metres, so they do not hit the
// surface they leave.
#define DDGI_SHADOW_RAY_OFFSET 0.01

// The probe images' largest side in texels, which every GPU with ray tracing supports for 2D images.
#define DDGI_MAX_ATLAS_SIDE 16384

// Invocations per workgroup of the trace pass, one ray each.
#define DDGI_TRACE_GROUP_SIZE 64

// Sampling the probes at a surface (render/gpu/ddgi_sampling.h, ddgi_sampling.glsl), the 2021 reference code's numbers.
// The self-shadow bias moves the point tested for visibility off the surface: mostly towards the viewer, a little along
// the normal, by this share of the smallest spacing times the tunable bias.
#define DDGI_BIAS_NORMAL_SHARE 0.2
#define DDGI_BIAS_VIEW_SHARE 0.8
#define DDGI_BIAS_SPACING_SHARE 0.75

// A probe behind the surface still counts this much (wrap shading), so small details inside a room keep some probes.
#define DDGI_WRAP_FLOOR 0.2

// Chebyshev's visibility bound is raised to this power to sharpen it, and never drops below the floor, so a point no
// probe can see still gets some light.
#define DDGI_VISIBILITY_POWER 3.0
#define DDGI_MIN_VISIBILITY 0.05

// A probe's weight is at least the smallest weight, and weights below the crush threshold shrink as their cube (scaled to
// meet at the threshold), so faint leaks, which the eye notices most in the dark, fade away.
#define DDGI_SMALLEST_WEIGHT 1e-6
#define DDGI_CRUSH_THRESHOLD 0.2

// Each trilinear factor is at least this, so a probe at a cell's far corner is never weighted to exactly nothing.
#define DDGI_MIN_TRILINEAR 0.001

// A volume lights points out to this many cells beyond its outer probes at full weight, then fades to nothing over the
// next DDGI_VOLUME_FADE cells, where the flat ambient light takes over.
#define DDGI_VOLUME_MARGIN 0.5
#define DDGI_VOLUME_FADE 0.5

// Cascades (render/gpu/probe_cascades.h): nested grids of probes following a point, each twice the spacing of the last.
// The most a frame may hold; each fades out over its last DDGI_CASCADE_FADE cells, ending DDGI_CASCADE_EDGE cells (and
// the self-shadow bias's reach) inside the nearest an outer plane of its window can be, so the cell round any point it
// lights never touches an outer plane, where a scroll brings in probes that are new.
#define DDGI_MAX_CASCADES 6
#define DDGI_CASCADE_FADE 1.0
#define DDGI_CASCADE_EDGE 1.0

// A probe that starts (scrolled in by a cascade, woken, newly vigilant) is seeded from the coarser cascades' light where
// it stands, which counts as this many frames of its own history and then fades out as its own rays come in. At most
// DDGI_MAX_SEEDS probes are seeded a frame; the rest start from their first frame's rays.
#define DDGI_SEED_FRAMES 4.0
#define DDGI_MAX_SEEDS 2048

// The main pass's specialization constant (constant_id) that switches sampling the probes on. Its pipelines without probes
// have none of the probe code, so pictures without probes are drawn exactly as before probes existed.
#define DDGI_SAMPLE_PROBES_CONSTANT 0

// The most point and spot lights a probe ray's hit may add (each costs a shadow ray); a setting picks how many up to this.
#define DDGI_MAX_HIT_LIGHTS 8

// The adaptive memory (render/gpu/ddgi.h, adaptiveProbeBlend): a change smaller than this share of the stored light is
// never taken for a real one, however steady the light (a texel in total darkness has no noise to measure against).
#define DDGI_CHANGE_FLOOR_SHARE 0.01

// The most frames a texel's history counts, which a half float holds exactly; by then the long memory's share is the
// smaller anyway.
#define DDGI_MAX_HISTORY 2048

// Sharing between neighbouring probes (render/gpu/ddgi.h, probesSeeEachOther): a probe sees its neighbour when its mean
// distance towards it reaches this share of the way there; a wall between them stops it far short.
#define DDGI_SHARE_VISIBLE_SHARE 0.9

// The neighbours each probe shares with: its six face neighbours (one along each way of each axis).
#define DDGI_SHARE_NEIGHBOURS 6

// Relocation and states (render/gpu/probe_relocation.h, probe_relocation.glsl), the 2021 reference code's rules. Each
// probe traces this many distance-only rays (a fixed spiral, so the result is the same every time) against the static
// level, up to this many times, moving after each.
#define DDGI_RELOCATION_RAYS 256
#define DDGI_RELOCATION_ITERATIONS 5

// Invocations per workgroup of the relocation pass, one workgroup per probe.
#define DDGI_RELOCATION_GROUP_SIZE 64

// A probe whose rays meet back faces more than this share of the time is inside something.
#define DDGI_INSIDE_BACK_FACE_SHARE 0.25

// The furthest a probe moves from its grid point, per axis, as a share of the spacing: inside its own cell, so the cell
// a point falls in still finds its probes by the grid.
#define DDGI_OFFSET_LIMIT 0.4

// A probe leaving through a back face stops this far short of the offset limit, in metres.
#define DDGI_ESCAPE_MARGIN 0.001

// A probe whose nearest front face is closer than this share of the smallest spacing moves up to the nudge share
// towards the farthest front face it sees, unless the two lie the same way (cosine above the last number: it only sees
// one surface, and moving could take it through).
#define DDGI_MIN_FRONT_FACE_SHARE 0.1
#define DDGI_FRONT_FACE_NUDGE 0.2
#define DDGI_SAME_SURFACE_COSINE 0.5

// What a probe does this frame (ProbeState): off (buried in the static level: never traced or read), sleeping (no
// static surface near: not traced, not read), awake (a sleeping probe near something that moves) and vigilant (a static
// surface near). Awake and vigilant probes are traced and read.
#define DDGI_PROBE_OFF 0
#define DDGI_PROBE_SLEEPING 1
#define DDGI_PROBE_AWAKE 2
#define DDGI_PROBE_VIGILANT 3

#endif
