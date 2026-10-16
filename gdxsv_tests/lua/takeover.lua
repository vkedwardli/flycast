-- Replay takeover: from the battle start, hold E2E_PARAMS.hold (Dreamcast buttons, an MCS
-- mask no recorded input has) and log "E2E held=<vblank>" once a player's slot in the
-- game's input buffer (InetBuf, key word big-endian at +2) shows it for E2E_PARAMS.run vblanks
-- in a row: the game takes this pad, not the recording (within E2E_PARAMS.window vblanks). Then press START (retry) and log
-- "E2E held2=<vblank>" the same way, else "none". E2E_PARAMS.max: vblanks to the battle.

local SCENE = 0x0c3d16d4  -- u16: 0x0702 battle

local function wait_held(p)
  local run = 0
  local seen = e2e.wait_until(function()
    local any = false
    for slot = 0, 3 do
      local base = p.inetbuf + slot * 10
      if (e2e.read8(base + 2) * 256 + e2e.read8(base + 3)) & p.mcs == p.mcs then any = true end
    end
    run = any and run + 1 or 0
    return run >= p.run
  end, p.window)
  return seen and e2e.frame or "none"
end

function main()
  local p = E2E_PARAMS
  if not e2e.wait_until(function() return e2e.read16(SCENE) == 0x0702 end, p.max) then return end
  e2e.log("battle", e2e.frame)
  flycast.input.pressButtons(1, p.hold)
  local held = wait_held(p)
  e2e.log("held", held)
  if held == "none" then return end
  flycast.input.releaseButtons(1, p.hold)
  e2e.wait(10)
  e2e.press(e2e.BTN.START, 5)
  e2e.wait(10)
  flycast.input.pressButtons(1, p.hold)
  e2e.log("held2", wait_held(p))
end
