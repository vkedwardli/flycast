# gdxsv_tests

End-to-end tests of the gdxsv features: each case runs the emulator with the
game and checks RAM values, logs or hash logs. One case covers one row of
[`docs/gdxsv/features.md`](../docs/gdxsv/features.md), whose Test and Control
columns name the case ids. A control (`<id>c`) runs the case's check on input
where the feature is absent and passes when the check catches it.

Python 3 standard library and Lua only (Pillow for image compares). Not run in
GitHub CI.

## Run

```
python gdxsv_tests/run.py --list
python gdxsv_tests/run.py --exe build/flycast.exe                 # smoke tier
python gdxsv_tests/run.py --exe build/flycast.exe --tier full --jobs 4
python gdxsv_tests/run.py --exe build/flycast.exe --cases R2,R2c
python gdxsv_tests/run.py --recheck gdxsv_tests/out --cases K1      # checks only, no flycast
```

- `--exe` or `GDXSV_EXE`: the flycast build to test.
- Results: `gdxsv_tests/out/` (`--out`), `summary.json` with the commit, one
  directory per case with the instances' logs. Exit code 0 when every case
  passed or was skipped.
- A case whose ROM is missing is skipped.
- `--recheck <out dir>`: each case saved there (with `--cases`: those and their
  controls) runs its checks on a copy of its files (in `<out dir>.recheck`) and
  prints each check with its values, in seconds. Instances, the lobby and the
  HTTP stub are not started: their exit, log and requests are read back from
  `probe.json`, `lbs.log`, `http_<n>.json`; what that run did not save is listed
  and defaults to exit 0 and no requests. Try a new or changed check here on an
  earlier run's directory before a run with flycast.

## Environment

| Variable | Content |
|---|---|
| `GDXSV_ROM_DIR` | `gdx-disc1/gdx-disc1.gdi`, `gdx-disc2/gdx-disc2.gdi`, NAOMI `naomi.zip`, `gundmxgd.zip`, `gundmxgd/gdl-0006.chd` |
| `GDXSV_DISC1`, `GDXSV_DISC2`, `GDXSV_NAOMI` | single files instead of `GDXSV_ROM_DIR` |
| `GDXSV_STATE_DIR` | boot savestates (`<rom>_99.state`), made on first use (default `out/state`) |
| `GDXSV_REPLAY_DIR` | corpus replays, downloaded on first use and checked against `corpus/replays.json` (default `out/replay`) |
| `GDXSV_REPLAY_JOBS` | replays R1 plays in parallel (default 4) |
| `GDXSV_SERVER_EXE` | O1-O5, S1: `gdxsv` from inada-s/gdxsv, built with `-ldflags "-X main.requiredFlycastVersion="`; S1 needs `-spectator_test_session` (gdxsv `master`) |
| `GDXSV_LOGIN_STATE` | O1-O5: a Disc 2 savestate at the network game menu (通信ゲームシステム) with a registered VMU |

The ROM, BIOS, savestates and replays are not in the repository.
Every instance gets `gdxsv:server=127.0.0.1`, `UploadReplay=no` and local
replay/live API URLs: no case reaches a production server. The online cases also
send the public IP, P2P and latency checks to a local fake server and turn UPnP off.

## Layout

| Path | Content |
|---|---|
| `run.py` | runner |
| `lib/emu.py` | launch and stop an instance (own data dir, config, ports); OS differences live here |
| `lib/case.py` | case registry, `Ctx.probe` (one instance with a Lua script), `result` |
| `lib/assets.py` | ROM, savestate and replay paths |
| `lib/hashlog.py`, `lib/replayfile.py` | per-frame hash logs; replay protobuf (round results, input cut) |
| `lua/e2e.lua` | helpers joined in front of every case script: frame waits, input, RAM reads, `E2E key=value` lines to `e2e.log` |
| `cases/*.py` | the cases |
| `corpus/replays.json` | replays of every format, with sha256 and the winners per round |
| `rollback/` | rollback suite `run_suite.py` (A1 to D8), see its README |

## Cases

| Id | Tier | Checks | Time (windows-mini) |
|---|---|---|---|
| P1 / P1c | smoke | game patches rewritten within 2 vblanks after the original values are written back (Disc 1, 2) | 3 s |
| L1 / L1c | smoke | translation patch: Disc 2 text per language | 6 s |
| N1 / N1c | smoke | NAOMI version boots: game id, code at `0x0c020000` | 21 s |
| C1 / C1c | smoke | no `emu.cfg`: the gdxsv defaults (`rend.Resolution` 720, `aica.Volume` 50, ...) still hold after the game start, read through Lua (C1c: an empty `emu.cfg`, the stock values) | 2 s |
| R2 / R2c | smoke | newest Disc 2 replay plays to the end with the recorded winners | 110-150 s |
| R1 | full | all corpus replays play to the end with the recorded winners | 470-520 s (4-5 replays in parallel) |
| W1 / W1c | full | widescreen matte edges and HUD offset per layout and viewport (formula of `WriteWidescreenPatch`) | 10 s |
| I1 / I1c | full | 4-peer local rollback match at GGPO delay 2 and 4: peer 1's B press reaches the confirmed input buffer 2 vblanks later at delay 4 | 130-260 s |
| O1 / O1c, O1t, O1i, O1p | full | Disc 2 logs in to a local `gdxsv lbs` (fresh SQLite, seeded user for the login key): platform info (payload `patch_id`, public IP, port test result), user login, plaza join; public IP and P2P test go to a local fake HTTP server, which sends the UDP "Hello" back (O1c: unknown login key; O1t: `gdxsv:server` 192.0.2.1, no connection; O1i: public IP answer 203.0.113.7 reaches the platform info; O1p: no UDP Hello, `Failed (Timeout)`) | 85 s each |
| O2 / O2c | full | O1, then a training lobby entry, chat force start, mobile suit select and battle: one player on the UDP backend through the local battle server (O2c: lobby without force start) | 170 s / 300 s |
| O3 / O3c | full | as O2 in a P2P lobby (`mcs_region` `p2p`): lobby sends the P2P matching, one-peer GGPO session, battle loads on the rollback backend (O3c: O3's checks on O2's lobby) | 170 s / 170 s |
| O4 / O4c | full | two clients (two login keys, same savestate) in one P2P lobby: both enter, one force-starts; P2P matching to both, two-peer GGPO session, each peer answers the other, both reach the battle (O4c: the second client only logs in) | 190 s / 190 s |
| O5 / O5c | full | O1, and the network payload (`gdxsv_patch/`) is in RAM: its install logged, current `patch_id` at the network menu, connection not refused (O5c: Lua zeroes `patch_id` every vblank: lobby connection refused, no login) | 85 s / 105 s |
| V1 / V1c | full | newest Disc 2 replay with `ReplayPOV` 2 and 4: the game stores player side POV-1 (`0x0c3d19d9`) and id POV (`0x0c391d92`) at vblank 4000 (V1c: no `ReplayPOV`, player 1) | 15 s |
| V2 / V2c | full | `replay_target_round=4` on a 5-round replay: one jump, rounds 4 and 5 play to the last input (V2c: from round 1) | 50 s / 110 s |
| V3 / V3c | full | `replay_target_frame=4000`: one seek landing on input 4000, then every round with the recorded winners (V3c: no target) | 110 s |
| V4 / V4c | full | `ReplayHideName`: the battle names the players ＰＩＬＯＴ０1-4 (`0x0c3d1b04`, 16 bytes each) (V4c: recorded names) | 5 s |
| V5 / V5c | full | `ReplayFourScreen=yes` in a window: the host starts 3 guests (one sync group); POV bytes per screen (guests write `e2e_pov<N>.log` via `lua/fourscreen.lua` in `emu.cfg`) are players 1-4; each guest log joined the host's group and plays its replay (V5c: no four-screen, every guest check fails) | 57 s |
| V6 / V6c | full | `replay_target_frame=3000 replay_takeover_frame=3000` in a window (the countdown runs from the pause-menu render): `StartTakeover` at the newest saved state before 3000 (within 60); `lua/takeover.lua` holds B X Y C Z (MCS 0xb003, in no corpus replay) and InetBuf shows it 60 vblanks in a row; START: `RetryTakeover` at the same input, then the mask again (V6c: no option, every takeover check fails) | 13 s / 28 s |
| K1 / K1c | full | `replay_target_frame=6000` with and without `SkipRenderingHack`: both land on input 6000 and play every round with the recorded winners; the hack seeks under 0.85 x the ms per frame (`JumpToKeyMsg` log line; windows-mini 2.0 vs 3.2) (K1c: off in both runs) | 410 s |
| F1 / F1c | full | `config:rend.FixedFrequency` 2, 4, 5 from the lobby savestate: 600 vblanks timed by the wall clock (`lua/rate.lua` marks, 10 ms poll) at 59.94, 50, 30 fps within 2 % (measured 59.91, 50.01, 30.00); F1c: `FixedFrequency=0 LimitFPS=no` ran 956 fps, the 59.94 check fails | 53 s / 2 s |
| H1 / H1c | full | `SlowIdleLoopHack` yes vs no, `LimitFPS=no` from the lobby savestate, 6000 vblanks timed like F1: the hack over 2 x the fps (measured 931-981 vs 318-333) (H1c: off in both runs) | 27 s / 39 s |
| F2 / F2c | full | `FixedFrequency=0 LimitFPS=yes`: the audio output paces the game, 59.94 fps within 2 % (measured 59.98); F2c: `LimitFPS=no` ran 958 fps | 13 s / 2 s |
| J1 / J1c, J2 | smoke (J1), full | `cases/joystick.py`: an SDL virtual gamepad (env `GDXSV_TEST_JOYSTICK`, a script of vblank-timed button/axis/hat changes) on the headless lobby savestate; `lua/joystick.lua` logs player 1's `kcode` and stick X. A pressed at vblank 300, released at 360: seen by 303 / 363, polling on (J1) and off (J2); J1c: the same checks on a script 10 vblanks late fail | 11 s |
| J3 / J3c | smoke (J3), full | a mapping file binds d-pad left/right to the left stick: left -32768, left + right 0, right 32767, none 0, each within 3 vblanks (J3c: right never pressed, the centre check fails) | 11 s |
| J4 / J4c | full | Windows: `input:UseXInput=no`, `input:UseDirectInput=no` log `Disabling XInput, using DirectInput` and `Disabling DirectInput` (SDL hints set); J4c: both `yes`, neither line | 2 s |
| B1 / B1c | full | `headless_loadstate=yes headless_probe_frames=300`: rc 0 by itself, `headless probe: exiting after 300 frames`, `Loaded state ..._99.state` (B1c: neither option, killed at 30 s, every check fails) | 6 s / 30 s |
| U1 / U1c | full | `cases/menu.py`: no ROM, a window (these run in the main menu), `ConnectionMedium=Wired` (the wireless popup would block the others); `gdxsv:UpdateCheckUrl` = a local release JSON with every platform zip: log `update check: latest gdxsv-99.0.0, newer 1, download 1` (U1c: `gdxsv-0.0.1`, `newer 0`). Judged by logs: a popup waits behind any other | 1 s / 1 s |
| X1 / X1c | full | `UseTexturePack=yes TexturePackChannel=e2e TexturePackUrl=<local>`: `/e2e.json` lists a 100 KB fake pack in 3 chunks (XXH64 in `lib/xxh64.py`); all fetched, `texture pack updated to e2e-1`, `T13306M.texp` = the bytes served; second start, same dir: `new 0`, no chunk fetched (X1c: one chunk hash wrong, not installed) | 3 s / 3 s |
| M1 / M1c | full | `ConnectionMedium=Wireless`: `wireless warning shown`; `VPN: e2e`: `VPN warning shown: VPN: e2e` (M1c: `Wired`, neither) | 3 s / 3 s |
| S1 / S1c | full | `rollback/live_rig.py`: 4 peers (30-80 ms, input delay 3, 2 rounds) upload to a local `gdxsv lbs -spectator_test_session`; its recording is one peer's replay, a spectator's state per step equals that replay played offline (S1c: peers without `TEST_SPECTATOR_LBS`, every check fails). Header replay made once per run | 410 s + 115 s |

W1 and W1c are known failures (`known_failure=`, reported `KNOWN`, exit code 0): on
the default `rend.ThreadedRendering=yes` the widescreen patch is written only when the
emulator stops between frames, so W1 reads stock values and W1c's check passes. With
`ThreadedRendering=no` both pass, and the runner reports `known failure passed: drop
the mark`. The fix is postponed: it may move the other patches' timing too.

## Adding a case

- A function in `cases/*.py` with `@case("<id>", "<what it checks>", tier=...)`,
  returning `result([{"what": ..., "ok": ...}, ...], **details)`.
- Lua: `lua/<name>.lua` defines `main()`, a coroutine resumed once per vblank;
  parameters arrive as `E2E_PARAMS`. Standard flycast Lua API only.
- Read what the check judges from files in `ctx.out` (logs, `e2e.log` values),
  so `--recheck` can replay it.
- Judge by RAM values, logs and hash logs; an image only for what is visual by
  nature, cropped and downscaled into `golden/`.
- Add a control and name both in `docs/gdxsv/features.md`.
- A failure a human chose not to fix yet: `known_failure="<why>"` on the case (and its control).
