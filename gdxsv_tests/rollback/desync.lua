-- Control probe (Disc 2): desync the one peer that loads it. From battle step 600 of the first
-- round, player 1's HP (PW + 0x182) is held at 1 on this peer only; the others play it as is.
-- Held every vblank, so a rollback to before the write cannot undo it.
local mem = flycast.memory
local PW = 0x0c3d1cd4
function cbVBlank()
  if mem.read8(PW + 5) ~= 3 or mem.read8(0x0c392059) ~= 0 then return end
  if mem.read32(0x0c3d17d8) >= 600 and mem.read16(PW + 0x182) > 1 then mem.write16(PW + 0x182, 1) end
end
