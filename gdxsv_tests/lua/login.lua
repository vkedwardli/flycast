-- Disc 2: from E2E_PARAMS.slot (the network menu savestate, registered VMU) log in
-- to the lobby server and on to the plaza map; the case checks the lobby's log.
-- Menu path from gdxsv-reveng tools/dc2_lobby.py "login".

local B = e2e.BTN

local STEPS = {  -- {buttons, vblanks to wait after the release}
  {B.DOWN, 20}, {B.A, 240}, {B.UP, 20}, {B.A, 240}, {B.LEFT, 20}, {B.A, 300}, {B.UP, 20},
  {B.A, 1500}, {B.A, 200}, {B.A, 180}, {B.UP, 20}, {B.A, 600},
}

function main()
  e2e.wait(E2E_PARAMS.boot)
  flycast.emulator.loadState(E2E_PARAMS.slot)
  e2e.wait(120)
  for _, s in ipairs(STEPS) do
    e2e.press(s[1], 20)
    e2e.wait(s[2])
  end
end
