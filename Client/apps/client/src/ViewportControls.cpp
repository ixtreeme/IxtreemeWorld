#include "ViewportControls.h"

#include "Debug.h"

#include <algorithm>
#include <cmath>

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
    const auto& m = camera.viewProjection.m;
    const float clipX = world.x * m[0] + world.y * m[4] + world.z * m[8] + m[12];
    const float clipY = world.x * m[1] + world.y * m[5] + world.z * m[9] + m[13];
    const float clipW = world.x * m[3] + world.y * m[7] + world.z * m[11] + m[15];
    if (clipW <= 0.0001f)
        return false;

    const float ndcX = clipX / clipW;
    const float ndcY = clipY / clipW;
    if (ndcX < -1.2f || ndcX > 1.2f || ndcY < -1.2f || ndcY > 1.2f)
        return false;

    outX = (ndcX * 0.5f + 0.5f) * static_cast<float>(width);
    outY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(height);
    return true;
}

WorldVec3 CameraForward(const WorldCamera& camera)
{
    return WorldNormalize(WorldSub(camera.target, camera.eye));
}

WorldVec3 CameraRight(const WorldCamera& camera)
{
    return WorldNormalize(WorldCross({0.0f, 1.0f, 0.0f}, CameraForward(camera)));
}

WorldVec3 CameraUp(const WorldCamera& camera)
{
    return WorldNormalize(WorldCross(CameraForward(camera), CameraRight(camera)));
}

WorldVec3 ScreenRayDirection(const WorldCamera& camera, uint32_t width, uint32_t height, int mouseX, int mouseY)
{
    const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const float ndcX = width != 0 ? (2.0f * static_cast<float>(mouseX) / static_cast<float>(width)) - 1.0f : 0.0f;
    const float ndcY = height != 0 ? 1.0f - (2.0f * static_cast<float>(mouseY) / static_cast<float>(height)) : 0.0f;
    constexpr float kTanHalfFov = 0.41421356237f; // tan(45deg / 2)
    return WorldNormalize(WorldAdd(
        WorldAdd(CameraForward(camera), WorldScale(CameraRight(camera), ndcX * aspect * kTanHalfFov)),
        WorldScale(CameraUp(camera), ndcY * kTanHalfFov)));
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

        WorldVec3 screenAnchor = WorldAdd(ServerMetersToDisplay(entity.position), {0.0f, 1.4f, 0.0f});
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

    const float length = std::sqrt(localX * localX + localZ * localZ);
    if (length > 0.0001f)
    {
        localX /= length;
        localZ /= length;
    }

    const WorldVec3 forward = {std::sin(cameraYawRadians), 0.0f, std::cos(cameraYawRadians)};
    const WorldVec3 right = {std::cos(cameraYawRadians), 0.0f, -std::sin(cameraYawRadians)};
    const WorldVec3 displayDir = WorldAdd(WorldScale(right, localX), WorldScale(forward, localZ));
    return std::atan2(displayDir.x, -displayDir.z);
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
    const WorldVec3 forward = WorldNormalize(WorldSub(target, eye));
    eye_ = eye;
    yaw_ = std::atan2(forward.x, forward.z);
    pitch_ = std::clamp(std::asin(std::clamp(forward.y, -1.0f, 1.0f)), -kMaxPitch, kMaxPitch);
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

    const float cosPitch = std::cos(pitch_);
    const WorldVec3 forward = {std::sin(yaw_) * cosPitch, std::sin(pitch_), std::cos(yaw_) * cosPitch};
    const WorldVec3 right = {std::cos(yaw_), 0.0f, -std::sin(yaw_)};
    WorldVec3 delta = WorldAdd(WorldAdd(WorldScale(forward, localZ), WorldScale(right, localX)),
        {0.0f, localY, 0.0f});
    if (WorldDot(delta, delta) > 0.0001f)
        delta = WorldNormalize(delta);

    const float speed = (movement.shift ? 22.0f : 9.0f) * speedScale_;
    eye_ = WorldAdd(eye_, WorldScale(delta, speed * dt));
}

WorldCamera FlyCameraController::BuildFlyCamera(uint32_t width, uint32_t height) const
{
    const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const float cosPitch = std::cos(pitch_);
    const WorldVec3 forward = {std::sin(yaw_) * cosPitch, std::sin(pitch_), std::cos(yaw_) * cosPitch};
    WorldCamera camera{};
    camera.eye = eye_;
    camera.target = WorldAdd(eye_, forward);
    const WorldMat4 view = WorldLookAt(camera.eye, camera.target, {0.0f, 1.0f, 0.0f});
    camera.nearPlane = 0.1f;
    camera.farPlane = 1000.0f;
    const WorldMat4 projection = WorldPerspective(45.0f * 3.1415926535f / 180.0f, aspect, camera.nearPlane, camera.farPlane);
    camera.viewProjection = WorldMultiply(view, projection);
    return camera;
}
