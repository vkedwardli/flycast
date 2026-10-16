-- Wait E2E_PARAMS.frame vblanks, then log each of E2E_PARAMS.options ({namespace, name})
-- as "E2E <namespace>.<name>=<value>" (flycast.config.<namespace>.<name>).

function main()
  e2e.wait(E2E_PARAMS.frame)
  for _, o in ipairs(E2E_PARAMS.options) do
    e2e.log(o[1] .. "." .. o[2], tostring(flycast.config[o[1]][o[2]]))
  end
end
