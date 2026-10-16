-- Log the game's per-player round counters each time they change:
-- "E2E c<N>=<slot>:<completed>,<wins>,<losses>,<draws>,<team>", N counting up.
-- Layout as gdxsv_round_counters.h: session pointer E2E_PARAMS.session,
-- player i (1-4) at session + i * 0x2c0, counters (u16 x 4) at +0x2b0, team (u8) at +0x2ba.

function main()
  local last, n = {}, 0
  while true do
    local session = e2e.read32(E2E_PARAMS.session)
    if session >= 0x0c000000 and session < 0x0d000000 - 5 * 0x2c0 then
      for i = 1, 4 do
        local p = session + i * 0x2c0
        local v = string.format("%d:%d,%d,%d,%d,%d", i - 1, e2e.read16(p + 0x2b0), e2e.read16(p + 0x2b2),
                                e2e.read16(p + 0x2b4), e2e.read16(p + 0x2b6), e2e.read8(p + 0x2ba))
        if v ~= last[i] then
          n = n + 1
          e2e.log("c" .. n, v)
          last[i] = v
        end
      end
    end
    coroutine.yield()
  end
end
