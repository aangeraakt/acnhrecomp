# Game versions

ACNHRecomp builds **any version** of Animal Crossing: New Horizons: the base game alone (1.0.0) or the base game with
any update, from a game folder holding that version (README, "Your game folder"). The recompiler and the runtime do
not depend on the version; only a few fixes that patch the game at fixed addresses do.

## How a version is picked

1. The game folder's `exefs/main` is the NSO of the version it holds (the update applied when there is one).
2. `tools/acnh_version.py` reads main's **build id** (NSO header 0x40, first 16 bytes) and looks it up in
   [`games/acnh/versions.txt`](../games/acnh/versions.txt). It writes `<folder>/version.ini`:
   ```ini
   build_id = 0948E48778171EE6D5D7A65813ED5FCC
   version = 2.0.7
   title_version = 1900544
   hooks = -
   ```
   The version name comes from versions.txt, else from the title version in the game folder's `game.ini` when it has
   one (release << 16).
3. The setup recompiles `main` with that build's hook list (`--hooks`), or with none. `gen/<module>/profile.txt`
   records what each module was recompiled with, so changing version or hook list recompiles only what changed.
4. At run time `games/acnh/game.cpp` recognises the known builds by their memory budget table and applies the fixes
   that do not go through a hook (the cSys budget, the clear-on-bind addresses) only there.

One version per folder: linking another game folder into a data folder clears what was made from the previous one
(`game_source.txt`). To keep several versions side by side, give each its own game folder and `--out` folder:

```sh
python3 tools/setup_acnh.py --game ~/ACNH-1.0.0 --out private/acnh-1.0.0
python3 tools/setup_acnh.py --game ~/ACNH-2.0.7 --out private/acnh-2.0.7
python3 tools/setup_acnh.py --game ~/ACNH-3.0.2 --out private/acnh-3.0.2
```

## What each version gets

| | 3.0.2 / 3.0.3 | 2.0.7 | 1.0.0 | Any other version |
|---|---|---|---|---|
| Recompile, shaders, build | Yes | Yes | Yes | Yes |
| Boot, island, saves, audio, input, settings screen (F1) | Yes | Yes | Yes | Yes (same runtime) |
| Native 60 FPS (`tools/setup_60fps.sh`, docs/NATIVE60.md) | Yes | No | No | No: play at 30 (or frame generation) |
| Wider aspect ratios beyond 16:9 (nvn_ultrawide.cpp) | Yes | No | No | 16:9 only |
| Mouse clicks on window buttons, lists, choices and waiting messages (pointer.cpp) | Yes | Yes | Yes | The game's own touch support only |
| PC keyboard typing in text entry (hle_hid.cpp) | Yes | Yes | No: the build does not read a USB keyboard there | Depends on the build |
| Phive half-built guard (game.cpp, a rare crash while Nook Inc. loads) | Yes | No | No | No |
| Clear-on-bind fix (game.cpp, nvn_cmd.cpp) | Yes | Yes | Yes | No: the picture stays black |
| cSys memory budget +64 MiB (game.cpp, per-build table) | Yes | Yes | Yes | No (a 3.8 MB allocation may fail at boot) |

## Status of the older builds (Windows, 2026-10-25)

- **NVN API version**: each SDK has its own and the game checks it (2.0.7 wants 53.313, 3.0.2 55.x). The runtime now
  asks the game's own driver (nvn_device.cpp) instead of answering 55.15 to everyone.
- **Profanity filter** (`nn::ngc`, hle_ngc.cpp): before 3.0.2 it mounts the console's NgWord archive at boot; it is
  now an empty filter.
- **2.0.7**: plays with picture and sound: the intro, Timmy's dialogues and the Nook Inc. scene in 3D are right. The
  engine clears through two paths: the per-target helpers (colour main+2741c90 -> ClearColor, lr main+2741e20; depth
  main+2741ef0 -> ClearDepthStencil, lr main+2742070), named in game.cpp's per-build table like 3.0.2's
  37fb4fc/37fb744, and a bind function (main+2748650) whose ClearColor/ClearDepthStencil are marked by hooks
  (games/acnh/v2_0_7.cpp). The first path alone was the black picture (the UI target wiped at each re-bind). The
  old hang (sound handles never stopped) was the audio renderer offset (hle_audio.cpp).
- **3.0.3**: 3.0.2's main with 16 bytes more code before main+31c3b50: the hooks from there on sit 0x10 further
  (games/acnh/hooks-3.0.3.txt, v3_0_3.cpp), the data (vtables, budget table) stays at 3.0.2's addresses; game.cpp
  tells the two apart by a word of code, since their budget tables are the same.
- **1.0.0**: the budget table is the same layout at main+0x29d4de0 with other values (more cSys); the clear helpers,
  touch, focus and button functions were found from 2.0.7's by their code (games/acnh/v1_0_0.cpp,
  hooks-1.0.0.txt). Its software keyboard does not read a USB keyboard (no nn::hid::GetKeyCode): names are typed by
  clicking the on-screen keys.

## Porting the fixes to another build

Each fix is an instruction address in main (`games/acnh/hooks.txt`) or a data address (`game.cpp`,
`nvn_ultrawide.cpp`, `pointer.cpp`). For another build:

1. Find each function in the other build's `analysis/main.bin`. The comments next to every address say what the
   function is (sead/nn class and method, strings it uses, the instruction hooked); byte patterns of the function's
   first instructions with relocated immediates masked usually find it directly.
2. Write the build's hook list as `games/acnh/hooks-<version>.txt` and add its line to `games/acnh/versions.txt`.
3. Make the data addresses a per-build table in the code that uses them (keyed by the same build id, read with
   `module_at(find_symbol("nnMain"))`), and gate each fix on its build being known.
4. Run the setup again: only `main` is recompiled.

## Known build ids

| Version | Title version | main build id |
|---|---|---|
| 1.0.0 (base game) | 0 | `7FC1BAFF976AECA414520CB89F4616CB` |
| 2.0.7 | 1900544 (release 29) | `0948E48778171EE6D5D7A65813ED5FCC` |
| 3.0.2 | 2162688 (release 33) | `FCD2BB238ABE99E925B4E452DF1F41F1` |
| 3.0.3 | 2228224 (release 34) | `FF1D1C05670DB6021C85B624A710B963` |

Add others as they are seen (`python tools/acnh_version.py <folder>` prints the build id).
