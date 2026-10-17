# gdxsv features and their tests

Every feature this fork adds to Flycast, with the tests that cover it.
The game is Mobile Suit Gundam: Federation vs. Zeon DX (Dreamcast, Disc 1 and
Disc 2). Every option is in [options.md](options.md); the changes to
upstream code and the fork point are in [upstream.md](upstream.md).

- A test is a unit test or a rig test: a script that runs one or more
  emulators with the game and checks the result. The game image is not in the
  repository.
- **Control** is the variant of a test that must fail. It shows the test can
  detect the problem.
- "none" means the feature has no automated test.
- This file does not record results. A result belongs to the commit it ran on.
- A feature's PR updates its row; "none" is stated, not left out.

## Where the tests are

| Location | Tests |
|---|---|
| `tests/src/Gdxsv*.cpp` | unit tests (`flycast_tests`, built with `-DBUILD_TESTING=ON`; CI does not run them) |
| `gdxsv_tests/` | `run.py` e2e cases by id (P1, R2, ...), tiers `smoke` and `full`; see its `README.md` |
| `gdxsv_tests/rollback/` | `run_suite.py` (rollback cases A1 to D8), `live_rig.py`, `fake_live_lbs.py`, `fuzz.py`; setup and pass criteria in its `README.md` |
| repository root | `run.py` (several local instances side by side) |

## Online play

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Network patch: an SH4 payload (`gdxsv_patch/`) replaces the game's modem stack; the game talks to the host through `gdx_rpc` | always for the game | O5: installed after the state load, the login runs through it | O5c (`patch_id` never current: no login) |
| Lobby connection over TCP (`gdxsv_backend_tcp`) | setting `server` (default `zdxsv.net`) | O1: login and plaza on a local `gdxsv lbs` | O1t (`server` with no lobby: no connection) |
| Login key, generated on first use | setting `loginkey` | O1: the lobby finds the account by the key | O1c (unknown key) |
| Platform info to the lobby (build, OS, CPU, machine id, wireless or not, local and public IPs, UDP port, UPnP and port test results) | always | O1: received by the lobby (CPU, payload `patch_id`, public IP, port test result) | O1i (another public IP answer shows up in it) |
| Wireless warning popup and VPN warning toast (connection medium) | main menu; `gdxsv:ConnectionMedium` overrides the detected adapter | M1: Wireless logs the popup, `VPN: e2e` the toast with the adapter | M1c |
| P2P connectivity test and public IP / UPnP port mapping | first lobby connection | O1: public IP and P2P test requests reach the local server, its UDP "Hello" makes the port test `Success` (UPnP off) | O1p (no Hello: `Failed (Timeout)`) |
| HTTPS latency test to the cloud regions | when the game goes online | none | none |
| Ping test to the battle peers, input delay from it | lobby battle | rollback cases A1 to D8 use it (`TEST_GGPO_DELAY` replaces the delay); I1: the delay setting reaches the game's input buffer | I1c |
| Relay through peers and relay servers, relay RTTs passed on between peers | lobby battle info | `run_suite.py` D2 to D8 (D4 to D8: local `gdxsv relay`); unit: `GdxsvPingPongTest.*` | D2c to D8c |
| Battle without GGPO over UDP (`gdxsv_backend_udp`) | battle info without rollback | O2: one player, local battle server | O2c |
| Network status OSD | in battle | none | none |

## Rollback netcode (GGPO)

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Rollback battle (`gdxsv_backend_rollback`, `core/network/ggpo.cpp`, `core/deps/ggpo`) | lobby battle | `run_suite.py`: A1 basic, A2 20 rounds (`--long`), B2/B3 threaded rendering on/off; O3: from a local gdxsv P2P lobby, O4: two clients | A1c, B2c, B3c, O3c, O4c |
| Compatibility with older releases | always | `run_suite.py` A3, A4 (`--old-release`, `--older-release`); live spectating: S3 (spectator), S4 (uploading peers) on the previous release | A3c, A4c, S3c, S4c |
| Timesync skips replayed in a rollback | always | `run_suite.py` B1 (`GGPO_TEST_LOG=1`) | B1c |
| Peer drop, session start timeout, re-battle cancel | always | `run_suite.py` C1, C2, C3 | C1c, C2c, C3c |
| Malformed packets dropped | always | `run_suite.py` D1 (`fuzz.py`) | D1c |
| Local test match without servers | `gdxsv:rbk_test=N/4`, `gdxsv:rand_input=SEED` | it is the tool of the cases above | none |
| SH4 `fmac` as `fmul` + `fadd` on every CPU and backend (same results across hosts) | CMake `GDXSV_FP_COMPAT` (on) | none | none |

## Replays

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Save a battle as a replay, upload it | settings `SaveReplay`, `UploadReplay`, `ReplayPath` | R3: a local rollback match (`RBK_SAVE_REPLAY`, never uploaded) saves one replay per peer, the same inputs, it plays to the end with the match's winner | R3c (`SaveReplay=no`) |
| Replay browser: Local, Server, Live tabs | menu "Live & Replays" | none | none |
| Play a replay file | menu, or `gdxsv:replay=FILE` | R1 (every replay format), R2: plays to the end with the recorded winners | R2c |
| Playback controls: pause (A), step or seek (Left/Right), speed (Up/Down), round jump, control bar | during a replay | unit: `GdxsvReplayInputTest.*`, `GdxsvReplayUi.*`; V2, V3: round jump and seek at start (`replay_target_round`, `replay_target_frame`) | V2c, V3c |
| Takeover: play on from a replay frame, retry with START | pause menu, or `gdxsv:replay_takeover_frame` | V6: the pad plays from the frame, START retries from it | V6c |
| Round results kept across seeks and round jumps | always | S2: a spectator joining a live battle in its last round sees each player's wins, losses and draws of the earlier rounds (`fake_live_lbs.py`); unit: `GdxsvRoundCountersTest.*` | S2c |
| Key display, Ally HP, hidden names, skip mobile suit selection | settings `ReplayKeyDisplay`, `ReplayShowAllyHP`, `ReplayHideName`, `ReplaySkipMsSelection` | V4: hidden names | V4c |
| Point of view | `gdxsv:ReplayPOV` | V1: the game plays as the chosen player | V1c |
| Four-screen replay: 4 processes in a 2x2 grid, kept on one frame | `gdxsv:ReplayFourScreen=yes` | V5: 3 guests on one sync group, each screen plays as its player | V5c |
| Audio rate control (resample up to 0.5 % to hold the queue half full) | replay and spectating without audio sync | none | none |
| Fast seek: output-only game functions return early on seek frames | setting `SkipRenderingHack` | K1: same seek and outcome, under 0.85 x the ms per frame without the hack | K1c |

## Live spectating

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Uplink: one battle peer streams its confirmed inputs to the lobby | lobby battle | S1 (`live_rig.py`): lobby's recording equals that peer's replay | S1c |
| Spectator: download, catch up, follow live | Live tab, or `gdxsv:spectate=CODE` | S1 (`live_rig.py`): each spectator's state per step equals the replay played offline; unit: `GdxsvSpectator.*` | S1c |
| Live auto-next (watch the next live battle) | setting `LiveAutoNext` | S5: `fake_live_lbs.py` with two battles, the spectator moves on to the second and joins it | S5c (`LiveAutoNext=no`) |

## Game and presentation

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Translation patch (Japanese, Cantonese, English) | setting `language` | L1 | L1c |
| Game patches (`gdxsv_patch.inc`), rewritten every vblank | always for the game | P1 (Disc 1, Disc 2) | P1c |
| NAOMI version (`gundmxgd`) boots | the NAOMI game files | N1 | N1c |
| Texture pack, downloaded and updated | settings `UseTexturePack`, `TexturePackChannel` | X1: a local channel, 3 chunks checked by hash and installed; the next start finds it current (`gdxsv:TexturePackUrl`); the pack is not loaded in a game (a fake pack) | X1c |
| Widescreen HUD layout | setting `WidescreenHudLayout` | W1 (known failure: not written under `rend.ThreadedRendering=yes`, the default) | W1c (known failure) |
| Slow idle loop hack | setting `SlowIdleLoopHack` | H1: unthrottled, over 2 x the fps without the hack | H1c |
| Projectile view (debug overlay, Disc 2) | setting `ProjectileView` (ini only) | none | none |
| Arcade slowdown (Disc 2) | compiled off (`GdxsvSlowdown::kEnabled`) | none | none |

## Frame pacing and input

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Fixed frame rate with a precise sleep (59.94, 60, 50, 30 Hz, or by cable) | setting `config:rend.FixedFrequency` | F1: 59.94, 50, 30 fps by the wall clock | F1c |
| Audio sync as a setting | setting `config:aica.LimitFPS` | F2: audio paces the game at 59.94 fps | F2c |
| Joystick polling on the emulation thread; XInput / DirectInput switches | settings `input:JoystickPolling`, `input:UseXInput`, `input:UseDirectInput` | J1 (polling on), J2 (off): SDL virtual gamepad; J4: the two switches (Windows) | J1c, J4c |
| Opposite directions cancel (SOCD) for the game | always for the game | J3: SDL virtual gamepad, d-pad bound to the left stick | J3c |
| gdxsv defaults on a new config file | first start | C1: the defaults hold after the game start | C1c (an `emu.cfg` already there) |
| Settings from an older release's `emu.cfg` (gdxsv-1.9.6 layout) | always | C2: flycast and gdxsv values hold after the game start | C2c (sections renamed) |

## Releases and tools

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Update check against the releases of `inada-s/flycast`, a popup offers the new version | on start (`gdxsv:UpdateCheckUrl`) | U1: a newer release on a local release API is offered with its download | U1c |
| Release build from a `gdxsv-*` tag | push a tag | the workflow itself | none |
| CI build (macOS, Linux, Windows MinGW) | every push and PR | the workflow itself | none |
| Headless mode (no window, no graphics API) | `gdxsv:headless=yes` | most rollback cases run headless | none |
| Boot probe for patch checks | `gdxsv:headless_loadstate=yes`, `gdxsv:headless_probe_frames=N` | B1: resumes the lobby state, exits rc 0 after N vblanks | B1c |
| Per-frame state hash log | `gdxsv:hashlog=yes` | Q1: a local rollback match logs every frame per peer, and each rollback's frames again (read by `gdxsv_tests/lib/hashlog.py`, rollback suite) | Q1c (`hashlog=no`) |
| `run.py` (several instances, replay, spectate, rollback test) and the `RunGame` workflow | `python run.py <function>` | none | none |
| Fixed host address of guest RAM on Windows (lower 28 bits equal the guest address) | always | none | none |
