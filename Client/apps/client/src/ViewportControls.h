#pragma once

#include "InputEvent.h"
#include "RuntimeSession.h"
#include "WorldCamera.h"

#include <cstdint>
#include <vector>

WorldVec3 ServerMetersToDisplay(RuntimeVec3 position);
RuntimeVec3 DisplayToServerMeters(WorldVec3 position);

bool ProjectWorldToScreen(const WorldCamera& camera,
                          WorldVec3 world,
                          uint32_t width,
                          uint32_t height,
                          float& outX,
                          float& outY);

WorldVec3 CameraForward(const WorldCamera& camera);
WorldVec3 CameraRight(const WorldCamera& camera);
WorldVec3 CameraUp(const WorldCamera& camera);
WorldVec3 ScreenRayDirection(const WorldCamera& camera, uint32_t width, uint32_t height, int mouseX, int mouseY);

std::uint32_t PickRenderEntityTarget(const std::vector<WorldRenderEntity>& entities,
                                     const WorldCamera& camera,
                                     uint32_t width,
                                     uint32_t height,
                                     int mouseX,
                                     int mouseY);

struct MovementInputState
{
    bool w = false;
    bool a = false;
    bool s = false;
    bool d = false;
    bool space = false;
    bool control = false;
    bool shift = false;
    bool wPhysicalDown = false;
    bool aPhysicalDown = false;
    bool sPhysicalDown = false;
    bool dPhysicalDown = false;
    bool spacePhysicalDown = false;
    bool controlPhysicalDown = false;
    bool shiftPhysicalDown = false;

    bool Apply(const InputEvent& event);
    bool ApplyGatedEdge(const InputEvent& event, bool gateOpen);
    void Clear();
    bool HasDirection() const { return w || a || s || d; }
    bool HasFlyVertical() const { return space || control; }
    float DirectionAngle(float cameraYawRadians) const;
    RuntimeMoveState State() const;
};

class FlyCameraController
{
public:
    struct Snapshot
    {
        WorldVec3 eye{};
        float yaw = 0.0f;
        float pitch = 0.0f;
    };

    void SetFreeCameraEnabled(bool enabled);
    bool HandleInput(const InputEvent& event);
    void Update(double dt, const MovementInputState& movement);
    WorldCamera BuildCamera(uint32_t width, uint32_t height) const;

    bool IsFreeCameraEnabled() const { return inputEnabled_; }
    float MovementYaw() const { return yaw_; }
    Snapshot SaveSnapshot() const;
    void RestoreSnapshot(const Snapshot& snapshot);
    void FocusOn(WorldVec3 target, float distance = 15.0f);

private:
    static constexpr float kMaxPitch = 80.0f * 3.1415926535f / 180.0f;

    void ApplyMouseDelta(float dx, float dy);
    void UpdateFly(float dt, const MovementInputState& movement);
    WorldCamera BuildFlyCamera(uint32_t width, uint32_t height) const;

    bool inputEnabled_ = true;
    bool dragActive_ = false;
    int lastMouseX_ = 0;
    int lastMouseY_ = 0;
    float yaw_ = 0.0f;
    float pitch_ = -25.0f * 3.1415926535f / 180.0f;
    float speedScale_ = 1.0f;
    WorldVec3 eye_ = {0.0f, 8.0f, -18.0f};
};
