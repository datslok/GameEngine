#pragma once

// What a query cost: how many items (colliders or triangles) the exact tests ran on, and how many grid cells a ray walked.
struct CollisionQueryStats {
    int tested = 0;
    int cellsWalked = 0;
};
