-- Demonstrates the rich engine API (v3): arrow-key input, raycast, animator params, spawn, collision.
-- Parameters: speed (units/sec, default 5).

function OnStart(self)
    self.speed = tonumber(self.params.speed) or 5.0
end

function OnUpdate(self, dt)
    local x, y, z = GetPosition(self.id)
    local v = self.speed * dt
    if IsKeyDown(Key.Up)    then z = z + v end
    if IsKeyDown(Key.Down)  then z = z - v end
    if IsKeyDown(Key.Left)  then x = x - v end
    if IsKeyDown(Key.Right) then x = x + v end
    SetPosition(self.id, x, y, z)

    -- Drive an animator parameter (no-op if this entity has no bound animator).
    SetAnimatorFloat(self.id, "Speed", v > 0 and self.speed or 0.0)

    -- Left-click: ray straight down for a ground check.
    if IsKeyDown(Key.MouseLeft) then
        local hit, entityId, px, py, pz, nx, ny, nz, dist = Raycast(x, y, z, 0, -1, 0, 50.0)
        if hit then
            Log("ground: entity=" .. entityId .. " dist=" .. string.format("%.2f", dist))
        end
    end
end

function OnCollision(self, other)
    Log("collided with entity " .. other)
end
