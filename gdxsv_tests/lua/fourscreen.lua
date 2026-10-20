-- Four-screen replay: the host and its three guests (same working directory, this
-- script through emu.cfg) each read the POV and warning gain at E2E_PARAMS.frame. A guest
-- writes them to e2e_pov<N>.log and never ends, so only the host writes e2e.log;
-- the host waits E2E_PARAMS.after more vblanks so the guests have written theirs.

local WARNING_GAIN = 0x0c1e7b6e -- Disc 2 common sound 7's gain selector.

function main()
  e2e.wait(E2E_PARAMS.frame)
  local side, id = e2e.read8(E2E_PARAMS.side), e2e.read8(E2E_PARAMS.id)
  local warning_gain = e2e.read8(WARNING_GAIN)
  if id ~= 1 then
    local f = io.open("e2e_pov" .. id .. ".log", "w")
    f:write("E2E side=" .. side .. "\nE2E id=" .. id .. "\nE2E warning_gain=" .. warning_gain .. "\n")
    f:close()
    while true do coroutine.yield() end
  end
  e2e.wait(E2E_PARAMS.after)
  e2e.log("side", side)
  e2e.log("id", id)
  e2e.log("warning_gain", warning_gain)
end
