#pragma once

/*
* Tuning for collision queries, owned by whatever owns the CollisionScene and read at every rebuild.
*/
struct CollisionSettings {
    // The broad phase grid's cube size: about the size of a typical collider (a character is 2 tall). Smaller cells
    // spread big colliders over many cells; bigger ones put many colliders in each cell.
    float gridCellSize = 2.0f;

    // Colliders whose box would cover more cells than this go on the grid's large list, checked by every query.
    int largeItemCells = 64;
};
