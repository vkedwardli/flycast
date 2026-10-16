-- Four-screen replay: the host and its three guests (same working directory, this
-- script through emu.cfg) each read the POV bytes at E2E_PARAMS.frame. A guest
-- writes them to e2e_pov<N>.log and never ends, so only the host writes e2e.log;
-- the host waits E2E_PARAMS.after more vblanks so the guests have written theirs.

function main()
  e2e.wait(E2E_PARAMS.frame)
  local side, id = e2e.read8(E2E_PARAMS.side), e2e.read8(E2E_PARAMS.id)
  if id ~= 1 then
    local f = io.open("e2e_pov" .. id .. ".log", "w")
    f:write("E2E side=" .. side .. "\nE2E id=" .. id .. "\n")
    f:close()
    while true do coroutine.yield() end
  end
  e2e.wait(E2E_PARAMS.after)
  e2e.log("side", side)
  e2e.log("id", id)
end
