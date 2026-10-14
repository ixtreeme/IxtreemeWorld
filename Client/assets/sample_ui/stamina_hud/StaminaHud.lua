-- StaminaHud: an MMO-style HUD (RmlUi) for the player character.
--
-- Stamina is consumed by sprinting (Shift, per second) and jumping (per jump) and reloads by itself a
-- moment after the last spending, faster while standing still. Run it dry and the character is
-- exhausted: no sprinting or jumping until it has recovered to `recoverAt`. The action bar's first
-- slot is a stamina potion (key 1 or a click): its charges reload one by one.
--
-- Use: put hud.rml + hud.rcss into the project's asset folder (default: ui/), then attach this script
-- to the entity that has the CharacterController.
-- Parameters (Script component -> Parameters), all optional:
--   document     = ui/hud.rml   the HUD document, relative to the asset folder
--   level        = 1            shown on the portrait
--   maxStamina   = 100
--   sprintCost   = 22           stamina per second while running
--   jumpCost     = 18           stamina per jump
--   regenRate    = 16           stamina per second while recovering (x1.5 standing still)
--   regenDelay   = 1.0          seconds after the last spending before recovery starts
--   recoverAt    = 35           an exhausted character may run and jump again from this much
--   potionAmount = 45           stamina one potion gives
--   potionCharges= 3
--   potionReload = 8            seconds to reload one potion charge

local function number(value, default)
    return tonumber(value) or default
end

-- Only touch the document when a value really changes (a layout pass is not free).
local function setText(self, id, text)
    if self.shown[id] ~= text then
        self.shown[id] = text
        UiSetText(self.doc, id, text)
    end
end

local function setProperty(self, id, property, value)
    local key = id .. "/" .. property
    if self.shown[key] ~= value then
        self.shown[key] = value
        UiSetProperty(self.doc, id, property, value)
    end
end

local function setClass(self, id, class, on)
    local key = id .. "." .. class
    if self.shown[key] ~= on then
        self.shown[key] = on
        UiSetClass(self.doc, id, class, on)
    end
end

local function showMessage(self, text)
    setText(self, "message", text)
    setClass(self, "message", "show", true)
    self.messageTimer = 1.6
end

local function spend(self, amount)
    if amount <= 0 then return end
    self.stamina = math.max(0, self.stamina - amount)
    self.sinceSpent = 0
    if self.stamina <= 0 and not self.exhausted then
        self.exhausted = true
        showMessage(self, "Exhausted!")
    end
end

local function usePotion(self)
    if self.charges <= 0 then
        showMessage(self, "No potion left, the next one is on its way")
        return
    end
    if self.stamina >= self.max then
        showMessage(self, "Stamina is already full")
        return
    end
    self.stamina = math.min(self.max, self.stamina + self.potionAmount)
    self.charges = self.charges - 1
    if self.stamina >= self.recoverAt then
        self.exhausted = false
    end
    self.slotFlash = 0.2
end

function OnStart(self)
    local p = self.params
    self.max = number(p.maxStamina, 100)
    self.sprintCost = number(p.sprintCost, 22)
    self.jumpCost = number(p.jumpCost, 18)
    self.regenRate = number(p.regenRate, 16)
    self.regenDelay = number(p.regenDelay, 1.0)
    self.recoverAt = number(p.recoverAt, 35)
    self.potionAmount = number(p.potionAmount, 45)
    self.potionCharges = number(p.potionCharges, 3)
    self.potionReload = number(p.potionReload, 8)

    self.stamina = self.max
    self.exhausted = false
    self.sinceSpent = 99
    self.charges = self.potionCharges
    self.reloadTimer = 0
    self.messageTimer = 0
    self.slotFlash = 0
    self.key1WasDown = false
    self.shown = {}

    self.doc = UiOpen(p.document or "ui/hud.rml")
    if self.doc == 0 then
        LogError("StaminaHud: the HUD document was not found (parameter 'document')")
        return
    end
    local name = GetName(self.id)
    setText(self, "player-name", name)
    setText(self, "portrait-initial", string.upper(string.sub(name, 1, 1)))
    setText(self, "player-level", tostring(math.floor(number(p.level, 1))))
end

function OnUpdate(self, dt)
    if self.doc == 0 then return end
    local character = GetCharacterState(self.id)

    -- Spending: what the character did in its last step.
    if character then
        if character.running then spend(self, self.sprintCost * dt) end
        if character.jumped then spend(self, self.jumpCost) end
        if self.exhausted and character.moving and IsKeyDown(Key.Shift) and self.messageTimer <= 0 then
            showMessage(self, "Too tired to run")
        end
    end

    -- Reloading: after a short breather, faster while standing still.
    self.sinceSpent = self.sinceSpent + dt
    local recovering = self.sinceSpent >= self.regenDelay and self.stamina < self.max
    if recovering then
        local rate = self.regenRate
        if character and not character.moving then rate = rate * 1.5 end
        self.stamina = math.min(self.max, self.stamina + rate * dt)
    end
    if self.exhausted and self.stamina >= self.recoverAt then
        self.exhausted = false
    end

    -- Potion: key 1 (on press, not while held) or a click on its slot.
    local key1 = IsKeyDown(Key.Num1)
    if (key1 and not self.key1WasDown) or UiConsumeClick(self.doc, "slot-potion") then
        usePotion(self)
    end
    self.key1WasDown = key1
    if self.charges < self.potionCharges then
        self.reloadTimer = self.reloadTimer + dt
        if self.reloadTimer >= self.potionReload then
            self.charges = self.charges + 1
            self.reloadTimer = 0
        end
    else
        self.reloadTimer = 0
    end

    -- What the character may do now.
    SetCharacterAbilities(self.id,
        not self.exhausted and self.stamina > 0,
        not self.exhausted and self.stamina >= self.jumpCost)

    -- HUD
    local fraction = self.stamina / self.max
    setProperty(self, "stamina-fill", "width", string.format("%.1f%%", fraction * 100))
    setText(self, "stamina-text", string.format("%d / %d", math.floor(self.stamina + 0.5), self.max))
    setClass(self, "stamina-bar", "exhausted", self.exhausted)
    setClass(self, "stamina-bar", "low", not self.exhausted and fraction < 0.3)
    setClass(self, "stamina-bar", "recovering", recovering and not self.exhausted)
    local state = ""
    if self.exhausted then state = "exhausted"
    elseif recovering then state = "recovering"
    elseif character and character.running then state = "sprinting" end
    setText(self, "stamina-state", state)

    setText(self, "slot-potion-count", tostring(self.charges))
    local reloading = self.charges < self.potionCharges
    local remaining = reloading and (1 - self.reloadTimer / self.potionReload) or 0
    setProperty(self, "slot-potion-cooldown", "height", string.format("%.0f%%", remaining * 100))
    setClass(self, "slot-potion", "no-charges", self.charges == 0)
    self.slotFlash = math.max(0, self.slotFlash - dt)
    setClass(self, "slot-potion", "used", self.slotFlash > 0)

    if self.messageTimer > 0 then
        self.messageTimer = self.messageTimer - dt
        if self.messageTimer <= 0 then setClass(self, "message", "show", false) end
    end
end

function OnDestroy(self)
    if self.doc and self.doc ~= 0 then
        UiClose(self.doc)
    end
end
