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
```

- `--exe` or `GDXSV_EXE`: the flycast build to test.
- Results: `gdxsv_tests/out/` (`--out`), `summary.json` with the commit, one
  directory per case with the instances' logs. Exit code 0 when every case
  passed or was skipped.
- A case whose ROM is missing is skipped.

## Environment

| Variable | Content |
|---|---|
| `GDXSV_ROM_DIR` | `gdx-disc1/gdx-disc1.gdi`, `gdx-disc2/gdx-disc2.gdi`, NAOMI `naomi.zip`, `gundmxgd.zip`, `gundmxgd/gdl-0006.chd` |
| `GDXSV_DISC1`, `GDXSV_DISC2`, `GDXSV_NAOMI` | single files instead of `GDXSV_ROM_DIR` |
| `GDXSV_STATE_DIR` | boot savestates (`<rom>_99.state`), made on first use (default `out/state`) |
| `GDXSV_REPLAY_DIR` | corpus replays, downloaded on first use and checked against `corpus/replays.json` (default `out/replay`) |
| `GDXSV_REPLAY_JOBS` | replays R1 plays in parallel (default 4) |
| `GDXSV_SERVER_EXE` | O1: `gdxsv` from inada-s/gdxsv, built with `-ldflags "-X main.requiredFlycastVersion="` |
| `GDXSV_LOGIN_STATE` | O1: a Disc 2 savestate at the network game menu (通信ゲームシステム) with a registered VMU |

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
| R2 / R2c | smoke | newest Disc 2 replay plays to the end with the recorded winners | 110-150 s |
| R1 | full | all corpus replays play to the end with the recorded winners | 470-520 s (4-5 replays in parallel) |
| W1 / W1c | full | widescreen matte edges and HUD offset per layout and viewport (formula of `WriteWidescreenPatch`) | 10 s |
| I1 / I1c | full | 4-peer local rollback match at GGPO delay 2 and 4: peer 1's B press reaches the confirmed input buffer 2 vblanks later at delay 4 | 130-260 s |
| O1 / O1c | full | Disc 2 logs in to a local `gdxsv lbs` (fresh SQLite, seeded user for the login key): platform info, user login, plaza join; public IP and P2P test go to a local fake HTTP server | 85-170 s |
| O2 / O2c | full | O1, then a training lobby entry, chat force start, mobile suit select and battle: one player on the UDP backend through the local battle server (O2c: lobby without force start) | 170 s / 300 s |
| O3 / O3c | full | as O2 in a P2P lobby (`mcs_region` `p2p`): lobby sends the P2P matching, one-peer GGPO session, battle loads on the rollback backend (O3c: O3's checks on O2's lobby) | 170 s / 170 s |

W1 fails on the default `rend.ThreadedRendering=yes`: the widescreen patch is
written only when the emulator stops between frames; W1c fails with it, since the
values are stock.

## Adding a case

- A function in `cases/*.py` with `@case("<id>", "<what it checks>", tier=...)`,
  returning `result([{"what": ..., "ok": ...}, ...], **details)`.
- Lua: `lua/<name>.lua` defines `main()`, a coroutine resumed once per vblank;
  parameters arrive as `E2E_PARAMS`. Standard flycast Lua API only.
- Judge by RAM values, logs and hash logs; an image only for what is visual by
  nature, cropped and downscaled into `golden/`.
- Add a control and name both in `docs/gdxsv/features.md`.
