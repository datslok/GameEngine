// The frame's cluster grid as clusters.glsl's ClusterGrid, from the frame's uniform block. Included by triangle.frag
// and cluster_lights.comp after frame_data.glsl and clusters.glsl.
#ifndef FRAME_CLUSTERS_GLSL
#define FRAME_CLUSTERS_GLSL

ClusterGrid frameClusterGrid() {
    return ClusterGrid(clusterTiles, clusterScreen, clusterSlicing, viewRight, viewUp, viewForward);
}

#endif
