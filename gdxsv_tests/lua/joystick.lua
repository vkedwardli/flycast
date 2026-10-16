-- Log player 1's buttons and left stick X, as the emulator gives them to the game, each time
-- they change: "E2E f<vblank>=<kcode hex> <joyx>", for E2E_PARAMS.frames vblanks.

function main()
  local last = nil
  for _ = 1, E2E_PARAMS.frames do
    local v = string.format("%08x %d", flycast.input.getButtons(1), flycast.input.getAxis(1, 1))
    if v ~= last then
      e2e.log("f" .. e2e.frame, v)
      last = v
    end
    coroutine.yield()
  end
end
