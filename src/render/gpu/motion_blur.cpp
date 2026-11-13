#include "render/gpu/motion_blur.h"
#include "math/smooth_step.h"
#include "render/gpu/column_major.h"
#include "render/gpu/taa_shared.h"

#include <algorithm>
#include <cmath>

// The object's whole motion less the camera's leaves what the object adds by moving itself.
Vec2 pixelMotion(Vec2 velocityTexel, Vec2 cameraMotionNdc, bool cameraMotionBlur) {
    const bool objectMoving = !(velocityTexel.x > static_cast<float>(TAA_NO_MOTION_TEST));

    if (cameraMotionBlur) {
        return objectMoving ? velocityTexel : cameraMotionNdc;
    }

    return objectMoving ? velocityTexel - cameraMotionNdc : Vec2{};
}

// NDC spans 2 across the screen, so half the size per unit; NDC's y counts up, pixels down.
Vec2 motionBlurHalfVector(Vec2 ndcMotion, Vec2 outputSize, float shutterShare) {
    const Vec2 half = Vec2{ndcMotion.x * 0.5f * outputSize.x, -ndcMotion.y * 0.5f * outputSize.y} * (shutterShare * 0.5f);
    const float length = half.length();
    const float longest = static_cast<float>(MOTION_BLUR_TILE);
    return length > longest ? half * (longest / length) : half;
}

Vec2 largerMotion(Vec2 a, Vec2 b) {
    return b.lengthSquared() > a.lengthSquared() ? b : a;
}

float motionBlurCone(float distance, float speed) {
    return std::clamp(1.0f - distance / speed, 0.0f, 1.0f);
}

float motionBlurCylinder(float distance, float speed) {
    return 1.0f - smoothStep(0.95f * speed, 1.05f * speed, distance);
}

float motionBlurNearer(float a, float b) {
    return std::clamp(1.0f - (a - b) / (static_cast<float>(MOTION_BLUR_DEPTH_SOFTNESS) * b), 0.0f, 1.0f);
}

// McGuire et al. 2012, figure 4: in front (f) and its own blur reaching the pixel, behind (b) and the pixel's blur
// reaching it, or both blurred over each other (the cylinders, counted twice).
float motionBlurSampleWeight(float distance, float pixelDepth, float sampleDepth, float pixelSpeed, float sampleSpeed) {
    const float inFront = motionBlurNearer(sampleDepth, pixelDepth);
    const float behind = motionBlurNearer(pixelDepth, sampleDepth);
    return inFront * motionBlurCone(distance, sampleSpeed) + behind * motionBlurCone(distance, pixelSpeed) +
           motionBlurCylinder(distance, sampleSpeed) * motionBlurCylinder(distance, pixelSpeed) * 2.0f;
}

// Evenly spaced inside -1..1, the middle index at 0 without noise; the noise moves them all by up to half a step.
float motionBlurOffset(int index, float noise) {
    const float jitter = noise - 0.5f;
    return -1.0f + 2.0f * (static_cast<float>(index) + jitter + 1.0f) / static_cast<float>(MOTION_BLUR_SAMPLES + 1);
}

Vec3 motionBlurAt(int x, int y, int width, int height, Vec2 neighbourMax, float noise, const MotionBlurPicture& picture, const MotionBlurPrepared& prepared) {
    const Vec3 colour = picture(x, y);
    const float least = static_cast<float>(MOTION_BLUR_LEAST_MOTION);

    if (neighbourMax.length() <= least) {
        return colour;
    }

    const MotionBlurTexel centre = prepared(x, y);
    const float pixelSpeed = std::max(centre.half.length(), least);
    float weights = 1.0f / pixelSpeed;
    Vec3 sum = colour * weights;
    const Vec2 place{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};

    for (int index = 0; index < MOTION_BLUR_SAMPLES; ++index) {
        if (index == (MOTION_BLUR_SAMPLES - 1) / 2) {
            continue;
        }

        const Vec2 at = place + neighbourMax * motionBlurOffset(index, noise);
        const int sx = std::clamp(static_cast<int>(std::floor(at.x)), 0, width - 1);
        const int sy = std::clamp(static_cast<int>(std::floor(at.y)), 0, height - 1);
        const MotionBlurTexel sample = prepared(sx, sy);
        const float distance = Vec2{static_cast<float>(sx - x), static_cast<float>(sy - y)}.length();
        const float weight = motionBlurSampleWeight(distance, centre.viewDepth, sample.viewDepth, pixelSpeed, std::max(sample.half.length(), least));
        weights += weight;
        sum = sum + picture(sx, sy) * weight;
    }

    return sum * (1.0f / weights);
}

RenderSize motionBlurTiles(RenderSize output) {
    const std::uint32_t tile = MOTION_BLUR_TILE;
    return RenderSize{(output.width + tile - 1) / tile, (output.height + tile - 1) / tile};
}

GpuMotionBlurPass packMotionBlurPass(const Mat4& inverseViewProjection, const Mat4& reprojection, RenderSize render, RenderSize output,
                                     const CameraSettings& camera) {
    GpuMotionBlurPass pass{};
    writeColumnMajor(inverseViewProjection, pass.inverseViewProjection);
    writeColumnMajor(reprojection, pass.reprojection);
    const auto sizeOf = [](RenderSize size, float (&into)[4]) {
        into[0] = static_cast<float>(size.width);
        into[1] = static_cast<float>(size.height);
        into[2] = 1.0f / static_cast<float>(size.width);
        into[3] = 1.0f / static_cast<float>(size.height);
    };
    sizeOf(render, pass.renderSize);
    sizeOf(output, pass.outputSize);
    pass.options[0] = camera.shutterAngle / 360.0f;
    pass.options[1] = camera.cameraMotionBlur ? 1.0f : 0.0f;
    return pass;
}
