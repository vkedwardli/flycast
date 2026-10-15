-- In a rollback battle: wait E2E_PARAMS.settle vblanks after the battle starts, hold B
-- (no rand_input presses it) and log how many vblanks later the game's confirmed input
-- buffer (InetBuf, one 10-byte KeyMsg1 per player, key word big-endian at +2) has it:
-- "E2E delay=<frames>", or "E2E delay=none" after E2E_PARAMS.window vblanks.

local DC_B, MCS_B = 0x0002, 0x2000

function main()
  local p = E2E_PARAMS
  e2e.wait_until(function() return e2e.read8(p.battle[1]) == 2 and e2e.read8(p.battle[2]) == 7 end)
  e2e.log("battle", e2e.frame)
  e2e.wait(p.settle)
  local start = e2e.frame
  flycast.input.pressButtons(1, DC_B)
  local seen = e2e.wait_until(function()
    for slot = 0, 3 do
      local base = p.inetbuf + slot * 10
      if (e2e.read8(base + 2) * 256 + e2e.read8(base + 3)) & MCS_B ~= 0 then
        e2e.log("slot", slot)
        return true
      end
    end
    return false
  end, p.window)
  flycast.input.releaseButtons(1, DC_B)
  e2e.log("delay", seen and (e2e.frame - start) or "none")
end
