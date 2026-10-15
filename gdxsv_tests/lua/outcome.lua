-- Log each round's result as the game decides it: "E2E round<N>=<outcome>", outcome as
-- gdxsv_round_counters::Outcome (winner byte E2E_PARAMS.winner, draw flag E2E_PARAMS.draw;
-- -1 = draw). Runs until the emulator exits.

function main()
  local last, n = 0, 0
  while true do
    local winner = e2e.read8(E2E_PARAMS.winner)
    local outcome = 0
    if winner ~= 0 then outcome = e2e.read8(E2E_PARAMS.draw) ~= 0 and -1 or winner end
    if outcome ~= last and outcome ~= 0 then
      -- a timeout sets the winner and the draw flag on different frames: same round
      if last == 0 then n = n + 1 end
      e2e.log("round" .. n, outcome)
    end
    last = outcome
    coroutine.yield()
  end
end
