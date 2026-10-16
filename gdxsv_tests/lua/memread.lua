-- Wait E2E_PARAMS.frame vblanks; write E2E_PARAMS.writes ({addr, width, value}),
-- wait E2E_PARAMS.after vblanks (0: read in the same vblank), then log each of
-- E2E_PARAMS.reads as "E2E <addr>:<width>=<value>" (widths 1, 2, 4: the integer;
-- other widths: hex bytes) and the game id.

local write = { [1] = flycast.memory.write8, [2] = flycast.memory.write16, [4] = flycast.memory.write32 }

function main()
  e2e.wait(E2E_PARAMS.frame)
  for _, w in ipairs(E2E_PARAMS.writes or {}) do write[w[2]](w[1], w[3]) end
  e2e.wait(E2E_PARAMS.after or 0)
  e2e.log("gameid", flycast.state.gameId)
  for _, r in ipairs(E2E_PARAMS.reads) do
    local addr, width = r[1], r[2]
    local key = string.format("0x%08x:%d", addr, width)
    if width == 1 then e2e.log(key, e2e.read8(addr))
    elseif width == 2 then e2e.log(key, e2e.read16(addr))
    elseif width == 4 then e2e.log(key, e2e.read32(addr))
    else e2e.log(key, e2e.hex(addr, width)) end
  end
end
