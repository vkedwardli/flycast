-- Frame rate: log "E2E t0" E2E_PARAMS.settle vblanks in and "E2E t1" E2E_PARAMS.frames later;
-- the case timestamps the lines as they appear (Lua has no portable fine clock).

function main()
  e2e.wait(E2E_PARAMS.settle)
  e2e.log("t0", e2e.frame)
  e2e.wait(E2E_PARAMS.frames)
  e2e.log("t1", e2e.frame)
end
