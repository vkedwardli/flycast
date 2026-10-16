-- Replay start: log "E2E battle=<vblank>" when the battle scene shows, then the hex bytes of
-- each E2E_PARAMS.reads ({addr, len}) E2E_PARAMS.after vblanks later. Gives up after E2E_PARAMS.max.

local SCENE = 0x0c3d16d4  -- u16: 0x0702 battle

function main()
  if not e2e.wait_until(function() return e2e.read16(SCENE) == 0x0702 end, E2E_PARAMS.max) then return end
  e2e.log("battle", e2e.frame)
  e2e.wait(E2E_PARAMS.after or 60)
  for _, r in ipairs(E2E_PARAMS.reads or {}) do
    e2e.log(string.format("0x%08x", r[1]), e2e.hex(r[1], r[2]))
  end
end
