-- Four-screen replay host (loaded through its --config, so its guests run without it).
-- Logs "E2E battle=<vblank>" once the battle is on screen. With E2E_PARAMS.controls it then
-- uses the replay controls on pad 1, E2E_PARAMS.gap vblanks apart: speed up, seek back, seek
-- forward, speed up again, logging each press; then waits for the next round's battle
-- ("E2E next=<vblank>") and E2E_PARAMS.after more vblanks. Without it, it never ends.

local SCENE = 0x0c3d16d4  -- u16: 0x0702 battle

local function battle() return e2e.read16(SCENE) == 0x0702 end

function main()
  local p = E2E_PARAMS
  if not e2e.wait_until(battle, p.max) then e2e.log("battle", "none") return end
  e2e.log("battle", e2e.frame)
  if not p.controls then
    while true do coroutine.yield() end
  end
  local steps = {{"speed_up", e2e.BTN.UP}, {"seek_back", e2e.BTN.LEFT}, {"seek_forward", e2e.BTN.RIGHT},
                 {"speed_up2", e2e.BTN.UP}}
  for _, step in ipairs(steps) do
    e2e.wait(p.gap)
    e2e.press(step[2], 5)
    e2e.log(step[1], e2e.frame)
  end
  if not e2e.wait_until(function() return not battle() end, p.max) then e2e.log("next", "none") return end
  if not e2e.wait_until(battle, p.max) then e2e.log("next", "none") return end
  e2e.log("next", e2e.frame)
  e2e.wait(p.after)
end
