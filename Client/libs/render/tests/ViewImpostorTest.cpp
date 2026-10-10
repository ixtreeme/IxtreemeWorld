#include "ViewImpostorRenderer.h"
#include "ViewImpostorPolicy.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

int main()
{
    unsigned checks = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
        ++checks;
    };
    try
    {
        WorldCamera camera;
        camera.eye = {0, 0, 0};
        camera.target = {0, 0, 1};
        camera.nearPlane = .1f;
        camera.farPlane = 1000;
        camera.viewProjection = WorldMultiply(WorldLookAt(camera.eye, camera.target, {0, 1, 0}),
                                              WorldPerspective(1.04719755f, 2, camera.nearPlane, camera.farPlane));
        const auto project = [&](WorldVec3 lo, WorldVec3 hi, float distance = 180.f) {
            return ViewImpostorRenderer::ProjectBounds(camera, lo, hi, 2000, 1000, distance, 256);
        };
        auto rectangle = project({-2, -2, 200}, {2, 2, 204});
        check(rectangle && rectangle->x < 1000 && rectangle->x + rectangle->width > 1000 && rectangle->y < 500 &&
                  rectangle->y + rectangle->height > 500,
              "perspective centre and guarded extent");
        check(rectangle && rectangle->width == rectangle->height, "native square crop in a wide view");
        check(!project({-2, -2, 179}, {2, 2, 400}), "distance uses closest surface rather than centre");
        check(!project({-200, -200, 200}, {200, 200, 204}), "oversized terrain/object stays geometry");
        check(!project({-2, -2, -204}, {2, 2, -200}), "behind-camera box stays geometry");
        check(!project({-1, -1, -.1f}, {1, 1, .2f}, 0), "near-plane crossing stays geometry");
        check(!project({-1, -1, 999}, {1, 1, 1001}), "far-plane crossing stays geometry");
        check(!project({-1, -1, -1}, {1, 1, 1}, 0), "camera inside terrain/object stays geometry");
        check(!project({-1, -1, 90}, {1, 1, 92}) && project({-1, -1, 90}, {1, 1, 92}, 80).has_value(),
              "separate character threshold");
        check(!project({2, -2, 200}, {-2, 2, 204}), "invalid bound ordering");
        check(!project({std::numeric_limits<float>::quiet_NaN(), -2, 200}, {2, 2, 204}), "NaN source bound");
        check(!project({-2, -2, 200}, {2, 2, 204}, INFINITY), "non-finite distance");
        check(!project({1000, -2, 200}, {1004, 2, 204}), "offscreen object");
        camera.viewProjection.m[0] = INFINITY;
        check(!project({-2, -2, 200}, {2, 2, 204}), "non-finite camera projection");
        check(ViewImpostorRenderer::Fresh(2.01, 2, 1.0 / 60), "pose fresh before the 60-Hz deadline");
        check(!ViewImpostorRenderer::Fresh(2.02, 2, 1.0 / 60), "expired pose must refresh or use geometry");
        check(!ViewImpostorRenderer::Fresh(1.9, 2, 1.0 / 60), "clock rewind invalidates pose");
        check(!ViewImpostorRenderer::Fresh(NAN, 2, 1.0 / 60), "invalid clock invalidates pose");
        check(!ViewImpostorRenderer::Fresh(2, -1, 1.0 / 60), "uncaptured entry never reused");
        const auto makeCamera = [](WorldVec3 eye, float yaw, float pitch = 0) {
            WorldCamera result;
            result.eye = eye;
            result.target = WorldAdd(eye, WorldForwardFromYawPitch(yaw, pitch));
            result.viewProjection = WorldMultiply(WorldLookAt(eye, result.target, {0, 1, 0}),
                                                  WorldPerspective(1.04719755f, 2, result.nearPlane, result.farPlane));
            return result;
        };
        const auto original = makeCamera({0, 0, 0}, 0);
        const auto turned = makeCamera({0, 0, 0}, .01f, .003f);
        const auto mapping = ViewImpostorRenderer::RotationMapping(original, turned);
        check(mapping.has_value(), "same-position yaw and pitch permit ray reprojection");
        const auto identity = ViewImpostorRenderer::RotationMapping(original, original);
        check(identity && std::abs(identity->m[0] - 1) < 1e-6 && std::abs(identity->m[15] - 1) < 1e-6,
              "identical camera gives the identity mapping");
        check(!ViewImpostorRenderer::RotationMapping(original, makeCamera({.001f, 0, 0}, .01f)),
              "even a small translation must not hide newly revealed surfaces");
        check(!ViewImpostorRenderer::RotationMapping(original, makeCamera({0, 0, 0}, .06f)),
              "large camera turn requests a fresh capture");
        auto invalid = original;
        invalid.viewProjection = {};
        check(!ViewImpostorRenderer::RotationMapping(original, invalid), "singular camera uses geometry");
        invalid.viewProjection = WorldIdentity();
        check(!ViewImpostorRenderer::RotationMapping(original, invalid), "orthographic camera uses geometry");
        invalid = turned;
        invalid.viewProjection.m[7] = NAN;
        check(!ViewImpostorRenderer::RotationMapping(original, invalid), "non-finite rotation mapping rejected");
        invalid = turned;
        invalid.target = invalid.eye;
        check(!ViewImpostorRenderer::RotationMapping(original, invalid), "invalid camera direction rejected");
        invalid = turned;
        invalid.farPlane = 2000;
        check(!ViewImpostorRenderer::RotationMapping(original, invalid), "changed clip range requires recapture");
        invalid = turned;
        invalid.viewProjection.m[12] += .1f;
        check(!ViewImpostorRenderer::RotationMapping(original, invalid),
              "declared eye must match the actual projection origin");
        const auto shiftedOriginal = makeCamera({10, 16, -250}, 0);
        check(ViewImpostorRenderer::RotationMapping(shiftedOriginal, makeCamera({10, 16, -250}, .01f)).has_value(),
              "translated world coordinate system still permits rotation about a common origin");
        const auto clip = [](const WorldMat4& matrix, WorldVec3 point) {
            std::array<double, 4> output{};
            const double input[] = {point.x, point.y, point.z, 1};
            for (unsigned column = 0; column < 4; ++column)
                for (unsigned row = 0; row < 4; ++row)
                    output[column] += input[row] * matrix.m[row * 4 + column];
            return output;
        };
        for (const WorldVec3 point : {WorldVec3{-2, -2, 200}, {2, 3, 400}, {20, 5, 750}})
        {
            const auto oldClip = clip(original.viewProjection, point), newClip = clip(turned.viewProjection, point);
            const double x = newClip[0] / newClip[3], y = newClip[1] / newClip[3],
                         sourceDepth = oldClip[2] / oldClip[3];
            const auto& h = *mapping;
            const double sx = x * h.m[0] + y * h.m[4] + h.m[12];
            const double sy = x * h.m[1] + y * h.m[5] + h.m[13];
            const double sw = x * h.m[3] + y * h.m[7] + h.m[15];
            check(std::abs(sx / sw - oldClip[0] / oldClip[3]) < 2e-6 &&
                      std::abs(sy / sw - oldClip[1] / oldClip[3]) < 2e-6,
                  "current ray maps to independently projected source pixel");
            const double depth =
                (x * h.m[2] + y * h.m[6] + h.m[14] - sourceDepth * sw) / (sourceDepth * h.m[11] - h.m[10]);
            check(std::abs(depth - newClip[2] / newClip[3]) < 2e-7,
                  "reprojected depth matches direct geometry projection");
        }
        const auto output =
            ViewImpostorRenderer::ProjectBounds(turned, {-2, -2, 200}, {2, 2, 204}, 2000, 1000, 180, 256);
        check(mapping && output &&
                  ViewImpostorRenderer::ReprojectionCovered(*mapping, {960, 460, 80, 80}, *output, 2000, 1000),
              "whole output crop fits rays already captured with a guard border");
        check(!ViewImpostorRenderer::ReprojectionCovered(*mapping, {1000, 500, 2, 2}, *output, 2000, 1000),
              "incomplete capture rejects the entire proxy instead of creating holes");
        check(!ViewImpostorRenderer::ReprojectionCovered(*identity, {0, 0, 2000, 1000}, {0, 0, 20, 20}, 2000, 1000),
              "revealed screen edge requires geometry");
        auto zoom = WorldIdentity();
        zoom.m[0] = 2;
        check(!ViewImpostorRenderer::ReprojectionCovered(zoom, {0, 0, 2000, 1000}, {990, 490, 20, 20}, 2000, 1000),
              "insufficient source resolution cannot be hidden by crop coverage");
        auto behind = WorldIdentity();
        behind.m[15] = -1;
        check(!ViewImpostorRenderer::ReprojectionCovered(behind, {0, 0, 2000, 1000}, *output, 2000, 1000),
              "negative homogeneous W must not reproject behind-camera rays");
        auto nonfinite = WorldIdentity();
        nonfinite.m[12] = NAN;
        check(!ViewImpostorRenderer::ReprojectionCovered(nonfinite, {0, 0, 2000, 1000}, *output, 2000, 1000),
              "invalid matrix cannot pass coverage by unordered comparisons");
        const auto calibrate = [](ViewImpostorPolicy& policy, double& time, double trialDelta, bool used) {
            policy.Observe(time, true);
            for (unsigned i = 0; i < 800; ++i)
            {
                time += .001;
                policy.Observe(time, true);
            }
            if (used)
                policy.RecordUse();
            for (unsigned i = 0; i < 2000 && policy.State() == ViewImpostorPolicy::Stage::Trial; ++i)
            {
                time += trialDelta;
                policy.Observe(time, true);
            }
        };
        ViewImpostorPolicy profitable;
        double policyTime = 10;
        calibrate(profitable, policyTime, .0008, true);
        check(profitable.State() == ViewImpostorPolicy::Stage::Enabled, "keep a measured whole-frame FPS improvement");
        check(!profitable.Observe(policyTime + .001, false) &&
                  profitable.State() == ViewImpostorPolicy::Stage::Baseline,
              "camera/light movement discards the previous calibration");
        ViewImpostorPolicy slower;
        policyTime = 10;
        calibrate(slower, policyTime, .00108, true);
        check(slower.State() == ViewImpostorPolicy::Stage::Geometry, "CPU-bound regressions return to geometry");
        ViewImpostorPolicy marginal;
        policyTime = 10;
        calibrate(marginal, policyTime, .00099, true);
        check(marginal.State() == ViewImpostorPolicy::Stage::Geometry,
              "require improvement beyond the two-percent noise margin");
        ViewImpostorPolicy unused;
        policyTime = 10;
        calibrate(unused, policyTime, .0008, false);
        check(unused.State() == ViewImpostorPolicy::Stage::Geometry,
              "empty/near-only scenes cannot win without a proxy draw");
        const double retryAt = policyTime + 10.01;
        for (; policyTime < retryAt;)
        {
            policyTime += .01;
            unused.Observe(policyTime, true);
        }
        check(unused.State() == ViewImpostorPolicy::Stage::Baseline,
              "reconsider a geometry-only scene after the cooldown");
        ViewImpostorPolicy interrupted;
        policyTime = 10;
        calibrate(interrupted, policyTime, .0008, true);
        check(!interrupted.Observe(policyTime - 1, true), "clock rewind cancels calibration");
        calibrate(interrupted, policyTime, .0008, true);
        check(!interrupted.Observe(policyTime + .5, true), "pause/upload stalls cancel calibration");
        check(!interrupted.Observe(NAN, true), "invalid clock cancels calibration");
        ViewImpostorPolicy slowFrames;
        policyTime = 10;
        slowFrames.Observe(policyTime, true);
        for (unsigned i = 0; i < 100 && slowFrames.State() == ViewImpostorPolicy::Stage::Baseline; ++i)
        {
            policyTime += .2;
            slowFrames.Observe(policyTime, true);
        }
        slowFrames.RecordUse();
        for (unsigned i = 0; i < 100 && slowFrames.State() == ViewImpostorPolicy::Stage::Trial; ++i)
        {
            policyTime += .05;
            slowFrames.Observe(policyTime, true);
        }
        check(slowFrames.State() == ViewImpostorPolicy::Stage::Enabled,
              "regular low-FPS scenes can calibrate rather than being mistaken for a pause");
        ViewImpostorRenderer renderer;
        check(!ViewImpostorRenderer::Active() && !renderer.IsCapturing(),
              "unbound view never captures shadow/reflection draws");
        check(!renderer.TryDraw(&renderer, 1, 0, ViewImpostorRenderer::Kind::Object, {}, {}, 1000,
                                [] { return 1000ull; }),
              "unbound renderer uses geometry");
        std::cout << "ViewImpostorTest: " << checks << " checks passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
