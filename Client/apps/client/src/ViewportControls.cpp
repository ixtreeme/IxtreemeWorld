#include "ViewportControls.h"

#include "Debug.h"

#include <algorithm>

namespace xm = ixtreeme::math;

WorldVec3 ServerMetersToDisplay(RuntimeVec3 position)
{
    return {position.x, position.z, -position.y};
}

RuntimeVec3 DisplayToServerMeters(WorldVec3 position)
{
    return {position.x, -position.z, position.y};
}

bool ProjectWorldToScreen(const WorldCamera& camera,
                          WorldVec3 world,
                          uint32_t width,
                          uint32_t height,
    float& outX,
    float& outY)
{
    return WorldProjectToScreen(camera, world, width, height, outX, outY);
}

WorldVec3 CameraForward(const WorldCamera& camera)
{
    return WorldCameraForward(camera);
}

WorldVec3 CameraRight(const WorldCamera& camera)
{
    return WorldCameraRight(camera);
}

WorldVec3 CameraUp(const WorldCamera& camera)
{
    return WorldCameraUp(camera);
}

WorldVec3 ScreenRayDirection(const WorldCamera& camera, uint32_t width, uint32_t height, int mouseX, int mouseY)
{
    return WorldScreenRayDirection(camera, width, height, mouseX, mouseY);
}

std::uint32_t PickRenderEntityTarget(const std::vector<WorldRenderEntity>& entities,
                                     const WorldCamera& camera,
                                     uint32_t width,
                                     uint32_t height,
                                     int mouseX,
                                     int mouseY)
{
    constexpr float kPickRadiusPixels = 74.0f;
    float bestDistanceSq = kPickRadiusPixels * kPickRadiusPixels;
    std::uint32_t bestNetId = 0;
    for (const auto& entity : entities)
    {
        if (entity.visualClassId == 0)
            continue;

        WorldVec3 screenAnchor = ServerMetersToDisplay(entity.position) + WorldVec3{0.0f, 1.4f, 0.0f};
        float sx = 0.0f;
        float sy = 0.0f;
        if (!ProjectWorldToScreen(camera, screenAnchor, width, height, sx, sy))
            continue;

        const float dx = sx - static_cast<float>(mouseX);
        const float dy = sy - static_cast<float>(mouseY);
        const float distanceSq = dx * dx + dy * dy;
        if (distanceSq < bestDistanceSq)
        {
            bestDistanceSq = distanceSq;
            bestNetId = entity.netId;
        }
    }
    return bestNetId;
}

bool MovementInputState::Apply(const InputEvent& event)
{
    if (event.type != InputEvent::KeyDown && event.type != InputEvent::KeyUp)
        return false;

    const bool pressed = event.type == InputEvent::KeyDown;
    switch (event.key)
    {
    case Key_W: w = pressed; return true;
    case Key_A: a = pressed; return true;
    case Key_S: s = pressed; return true;
    case Key_D: d = pressed; return true;
    case Key_Space: space = pressed; return true;
    case Key_Control: control = pressed; return true;
    case Key_Shift: shift = pressed; return true;
    default: return false;
    }
}

bool MovementInputState::ApplyGatedEdge(const InputEvent& event, bool gateOpen)
{
    if (event.type != InputEvent::KeyDown && event.type != InputEvent::KeyUp)
        return false;

    const auto updateKey = [&](bool& active, bool& physicalDown) {
        if (event.type == InputEvent::KeyDown)
        {
            if (!physicalDown && gateOpen)
                active = true;
            physicalDown = true;
        }
        else
        {
            physicalDown = false;
            active = false;
        }
    };

    switch (event.key)
    {
    case Key_W: updateKey(w, wPhysicalDown); return true;
    case Key_A: updateKey(a, aPhysicalDown); return true;
    case Key_S: updateKey(s, sPhysicalDown); return true;
    case Key_D: updateKey(d, dPhysicalDown); return true;
    case Key_Space: updateKey(space, spacePhysicalDown); return true;
    case Key_Control: updateKey(control, controlPhysicalDown); return true;
    case Key_Shift: updateKey(shift, shiftPhysicalDown); return true;
    default: return false;
    }
}

void MovementInputState::Clear()
{
    w = false;
    a = false;
    s = false;
    d = false;
    space = false;
    control = false;
    shift = false;
    wPhysicalDown = false;
    aPhysicalDown = false;
    sPhysicalDown = false;
    dPhysicalDown = false;
    spacePhysicalDown = false;
    controlPhysicalDown = false;
    shiftPhysicalDown = false;
}

float MovementInputState::DirectionAngle(float cameraYawRadians) const
{
    float localX = 0.0f;
    float localZ = 0.0f;
    if (w) localZ += 1.0f;
    if (s) localZ -= 1.0f;
    if (d) localX += 1.0f;
    if (a) localX -= 1.0f;

    const float lengthSq = localX * localX + localZ * localZ;
    if (lengthSq > 0.0001f)
    {
        const float length = xm::Sqrt(lengthSq);
        localX /= length;
        localZ /= length;
    }

    const WorldVec3 forward = WorldForwardFromYawPitch(cameraYawRadians, 0.0f);
    const WorldVec3 right = WorldRightFromYaw(cameraYawRadians);
    const WorldVec3 displayDir = right * localX + forward * localZ;
    return xm::Atan2(displayDir.x, -displayDir.z);
}

RuntimeMoveState MovementInputState::State() const
{
    if (!HasDirection())
        return RuntimeMoveState::Idle;
    return shift ? RuntimeMoveState::Running : RuntimeMoveState::Walking;
}

void FlyCameraController::SetFreeCameraEnabled(bool enabled)
{
    if (enabled == inputEnabled_)
        return;

    inputEnabled_ = enabled;
    dragActive_ = false;
    Tracenf("[CAMERA] mode=free enabled=%d", inputEnabled_ ? 1 : 0);
}

bool FlyCameraController::HandleInput(const InputEvent& event)
{
    if (!inputEnabled_)
        return false;

    switch (event.type)
    {
    case InputEvent::MouseDown:
        dragActive_ = true;
        lastMouseX_ = event.x;
        lastMouseY_ = event.y;
        return true;
    case InputEvent::MouseUp:
        dragActive_ = false;
        return true;
    case InputEvent::MouseMove:
        if (!dragActive_)
            return false;
        ApplyMouseDelta(static_cast<float>(event.x - lastMouseX_),
            static_cast<float>(event.y - lastMouseY_));
        lastMouseX_ = event.x;
        lastMouseY_ = event.y;
        return true;
    case InputEvent::MouseWheel:
        speedScale_ = std::clamp(speedScale_ + static_cast<float>(event.wheelDelta) / 120.0f * 0.15f, 0.25f, 4.0f);
        return true;
    default:
        return false;
    }
}

void FlyCameraController::Update(double dt, const MovementInputState& movement)
{
    if (!inputEnabled_)
        return;
    const float frameDt = static_cast<float>(std::clamp(dt, 0.0, 0.05));
    UpdateFly(frameDt, movement);
}

WorldCamera FlyCameraController::BuildCamera(uint32_t width, uint32_t height) const
{
    return BuildFlyCamera(width, height);
}

FlyCameraController::Snapshot FlyCameraController::SaveSnapshot() const
{
    return Snapshot{eye_, yaw_, pitch_};
}

void FlyCameraController::RestoreSnapshot(const Snapshot& snapshot)
{
    eye_ = snapshot.eye;
    yaw_ = snapshot.yaw;
    pitch_ = snapshot.pitch;
    dragActive_ = false;
}

void FlyCameraController::FocusOn(WorldVec3 target, float distance)
{
    inputEnabled_ = true;
    const WorldVec3 eye = {target.x, target.y + 5.0f, target.z - distance};
    const WorldVec3 forward = xm::Normalize(target - eye);
    eye_ = eye;
    yaw_ = WorldYawFromDirection(forward);
    pitch_ = std::clamp(WorldPitchFromDirection(forward), -kMaxPitch, kMaxPitch);
    dragActive_ = false;
    Tracenf("[HIERARCHY] Focused camera on target: %.2f, %.2f, %.2f", target.x, target.y, target.z);
}

void FlyCameraController::ApplyMouseDelta(float dx, float dy)
{
    constexpr float kSensitivity = 0.0045f;
    yaw_ += dx * kSensitivity;
    pitch_ = std::clamp(pitch_ + dy * kSensitivity, -kMaxPitch, kMaxPitch);
}

void FlyCameraController::UpdateFly(float dt, const MovementInputState& movement)
{
    float localX = 0.0f;
    float localZ = 0.0f;
    float localY = 0.0f;
    if (movement.w) localZ += 1.0f;
    if (movement.s) localZ -= 1.0f;
    if (movement.d) localX += 1.0f;
    if (movement.a) localX -= 1.0f;
    if (movement.space) localY += 1.0f;
    if (movement.control) localY -= 1.0f;

    const WorldVec3 forward = WorldForwardFromYawPitch(yaw_, pitch_);
    const WorldVec3 right = WorldRightFromYaw(yaw_);
    WorldVec3 delta = forward * localZ + right * localX + WorldVec3{0.0f, localY, 0.0f};
    if (xm::Dot(delta, delta) > 0.0001f)
        delta = xm::Normalize(delta);

    const float speed = (movement.shift ? 22.0f : 9.0f) * speedScale_;
    eye_ = eye_ + delta * (speed * dt);
}

WorldCamera FlyCameraController::BuildFlyCamera(uint32_t width, uint32_t height) const
{
    const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const WorldVec3 forward = WorldForwardFromYawPitch(yaw_, pitch_);
    WorldCamera camera{};
    camera.eye = eye_;
    camera.target = eye_ + forward;
    const WorldMat4 view = WorldLookAt(camera.eye, camera.target, {0.0f, 1.0f, 0.0f});
    camera.nearPlane = 0.1f;
    camera.farPlane = 1000.0f;
    const WorldMat4 projection = WorldPerspective(xm::DegreesToRadians(45.0f), aspect, camera.nearPlane, camera.farPlane);
    camera.viewProjection = WorldMultiply(view, projection);
    return camera;
}
