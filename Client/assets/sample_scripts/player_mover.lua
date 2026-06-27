-- PlayerMover: WASD planar movement, Space to rise, right-mouse-drag to yaw.
-- Parameters (Script component -> Parameters):
--   speed     = units/second        (default 5)
--   lookSpeed = yaw degrees/second  (default 90)
-- Input note: without a player CharacterController in the scene, Play uses the free-fly camera and
-- WASD drives that; with one present, WASD reaches this script. See the Lua scripting notes.

function OnStart(self)
    self.speed = tonumber(self.params.speed) or 5.0
    self.lookSpeed = tonumber(self.params.lookSpeed) or 90.0
end

function OnUpdate(self, dt)
    local x, y, z = GetPosition(self.id)
    local v = self.speed * dt
    if IsKeyDown(Key.W) then z = z + v end
    if IsKeyDown(Key.S) then z = z - v end
    if IsKeyDown(Key.D) then x = x + v end
    if IsKeyDown(Key.A) then x = x - v end
    if IsKeyDown("Space") then y = y + v end   -- string form works too
    SetPosition(self.id, x, y, z)

    if IsKeyDown(Key.MouseRight) then
        local dx, _ = MouseDelta()
        local rx, ry, rz = GetRotation(self.id)
        SetRotation(self.id, rx, ry - dx * self.lookSpeed * dt, rz)
    end
end

function OnCollision(self, other)
    Log("player hit entity " .. other)
end
