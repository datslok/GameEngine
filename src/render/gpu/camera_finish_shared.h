#ifndef RENDER_GPU_CAMERA_FINISH_SHARED_H
#define RENDER_GPU_CAMERA_FINISH_SHARED_H

// Numbers the camera's finish (render/gpu/camera_finish.h) shares with GLSL (camera_finish.glsl, tone_map.frag). Only
// #defines of plain numbers, since GLSL reads them too.

// The grade's contrast turns about this linear brightness, mid grey (18%), which it leaves where it is.
#define CAMERA_MID_GREY 0.18

// Film grain's noise turns with the frame number taken modulo this, so it fits a float exactly.
#define CAMERA_FRAME_CYCLE 65536

#endif
