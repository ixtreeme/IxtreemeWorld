-- Spinner: rotates the entity around its Y axis.
-- Parameter (Script component -> Parameters):  speed  = degrees/second (default 90).
-- Drag this .lua into the editor's asset browser, then on an entity add a Script component,
-- set Backend = Lua, pick this script, and press Play.

function OnStart(self)
    self.speed = tonumber(self.params.speed) or 90.0
    Log("spinner start on entity " .. self.id)
end

function OnUpdate(self, dt)
    local rx, ry, rz = GetRotation(self.id)   -- Euler degrees, three return values
    ry = (ry + self.speed * dt) % 360.0
    SetRotation(self.id, rx, ry, rz)
end

function OnDestroy(self)
    Log("spinner stop on entity " .. self.id)
end
