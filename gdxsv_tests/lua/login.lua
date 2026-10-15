-- Disc 2: from E2E_PARAMS.slot (the network menu savestate, registered VMU) log in
-- to the lobby server and on to the plaza map; the case checks the lobby's log.
-- E2E_PARAMS.battle: then enter lobby 2 (a training lobby), force-start with the chat
-- message "あ", pick a mobile suit and log "battle=1" once the battle scene is reached.
-- Menu paths from gdxsv-reveng tools/dc2_lobby.py.

local B = e2e.BTN
local SCENE = 0x0c3d16d4  -- u16: 0x0102 mobile suit select, 0x0702 battle

local LOGIN = {  -- {buttons, vblanks held, vblanks to wait after the release}
  {B.DOWN, 20, 20}, {B.A, 20, 240}, {B.UP, 20, 20}, {B.A, 20, 240}, {B.LEFT, 20, 20}, {B.A, 20, 300},
  {B.UP, 20, 20}, {B.A, 20, 1500}, {B.A, 20, 200}, {B.A, 20, 180}, {B.UP, 20, 20}, {B.A, 20, 600},
}
-- Plaza map, cursor on the info desk -> lobby 2 -> entry for team Federation (auto partner).
local LOBBY = {
  {B.UP, 38, 10}, {B.LEFT, 6, 30}, {B.A, 20, 300}, {B.A, 20, 120}, {B.UP, 20, 20}, {B.A, 20, 180},
}
-- Start, Y = on-screen keyboard; R, A = "あ"; 6 x Down, 2 x Right = OK; A commits, A sends.
local CHAT = { {B.START, 10, 60}, {B.Y, 10, 60}, {B.RIGHT, 15, 15}, {B.A, 15, 20} }
for _ = 1, 6 do CHAT[#CHAT + 1] = {B.DOWN, 15, 15} end
for _, s in ipairs({ {B.RIGHT, 15, 15}, {B.RIGHT, 15, 15}, {B.A, 15, 40}, {B.A, 15, 60} }) do
  CHAT[#CHAT + 1] = s
end

local function run(steps)
  for _, s in ipairs(steps) do
    e2e.press(s[1], s[2])
    e2e.wait(s[3])
  end
end

local function scene() return e2e.read16(SCENE) end

function main()
  e2e.wait(E2E_PARAMS.boot)
  flycast.emulator.loadState(E2E_PARAMS.slot)
  e2e.wait(120)
  run(LOGIN)
  if not E2E_PARAMS.battle then return end
  run(LOBBY)
  run(CHAT)
  -- The server's countdown runs in wall time: wait for the scenes, not a frame count.
  if not e2e.wait_until(function() return scene() == 0x0102 end, 12000) then
    e2e.log("battle", 0)
    return
  end
  e2e.wait(120)  -- mobile suit select ignores A for its first 120 vblanks
  e2e.press(B.A, 15)
  for _ = 1, 8 do  -- A through the briefing until the battle starts
    if e2e.wait_until(function() return scene() == 0x0702 end, 900) then
      e2e.log("battle", 1)
      e2e.wait(E2E_PARAMS.in_battle or 300)
      return
    end
    e2e.press(B.A, 10)
  end
  e2e.log("battle", 0)
end
