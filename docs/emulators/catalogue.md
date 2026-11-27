# Switch emulator catalogue (state checked 2026-11-26)

Scope: Nintendo Switch emulators in real use on Windows, Linux, macOS, Android, iOS.
Every fact names its source. `(U)` = unverified / inferred, not read from the project's own source or release today.

Source abbreviations used below:

- **EDEN-SRC** = `https://git.eden-emu.dev/eden-emu/eden` branch `master` (files named inline)
- **RYU-SRC** = `https://git.ryujinx.app/projects/Ryubing` branch `master`
- **KEN-SRC** = `https://git.ryujinx.app/projects/Kenji-NX` (`master` desktop, `libryujinx_bionic` Android)
- **SUYU-SRC** = `https://github.com/suyu-emu/suyu-main` default branch
- **ESDE** = ES-DE `resources/systems/<os>/es_find_rules.xml` + `es_systems.xml` on `gitlab.com/es-de/emulationstation-de` master
- **DAIJ** = Daijisho `platforms/NintendoSwitch.json` on `github.com/TapiocaFox/Daijishou` main
- **APK** = read from the `AndroidManifest.xml` inside the APK downloaded today
- **ZIP** = file listing of the archive downloaded today

## 1. Status overview

| Emulator | Family | Platforms | Status 2026-11 | Last release | Home / builds |
|---|---|---|---|---|---|
| **Eden** | yuzu | Win, Linux, macOS, Android | Active (repo commit 2026-11-24) | stable v0.2.1, 2026-06-01; nightlies | `eden-emu.dev`; source `git.eden-emu.dev/eden-emu/eden`; builds on `stable.eden-emu.dev` (linked from the Forgejo release); nightlies `git.eden-emu.dev/eden-ci/nightly` |
| **Ryubing** (Ryujinx fork) | Ryujinx | Win, Linux, macOS | Maintained; stable is old, canary is current | stable 1.3.3, 2025-10-11; canary 1.3.356, 2026-11-23 | `ryujinx.app`; source + builds `git.ryujinx.app/projects/Ryubing`; canary `git.ryujinx.app/Ryubing/Canary` |
| **Kenji-NX** | Ryujinx | Win, Linux, macOS, Android | Active source (2026-11-21), few releases | desktop 2.0.5, 2025-10-06; Android 2.1.0-pr.2, 2026-02-13 | `git.ryujinx.app/projects/Kenji-NX`; Android builds `git.ryujinx.app/Kenji-NX/Android` |
| **Suyu** ("suyu-main", revived) | yuzu (now rebased on Eden) | Win, Linux, macOS, Android | Active again since 2026-09 | v0.1.4, 2026-10-12 | `github.com/suyu-emu/suyu-main` (the original 2024 org) |
| **Drippu** ("suyu experimental") | yuzu/Eden | Win, Linux, macOS, FreeBSD, Android | Active, pre-release only | v0.0.5 automated build 2026-11-25 | `github.com/suyu-emu/drippu` |
| **Lemon** | yuzu (Eden fork) | Android only | Active, new (repo created 2026-10-02) | v1.6.0, 2026-11-23 | `git.lemon-emu.org/lemon/Lemon-Project` |
| **Citron Neo** | yuzu (Citron continuation) | Win, Linux, macOS, Android | Alive but homeless: GitHub repos removed 2026-11 | 2026-04-27 (per its site) | site `citron-neo.github.io` (= `citron-neo.org`) still links the removed GitHub repos; reported new host `git.mythrax-rs.org/Citron-NEO/emulator` not confirmed by the project's site |
| **Citron** (original) | yuzu | Win, Linux, macOS, Android | Dead: `citron-emu.org` and `git.citron-emu.org` no longer resolve | 0.12.25, 2025-12 (U) | none; archived source copy at `git.ryujinx.app/archive/citron` |
| **MeloNX** | Ryujinx | iOS only | Active | 2.7, 2026-10-11 (`.ipa`) | `melonx.org` -> `starforgejo.dev/projects/MeloNX` (old copy on `git.ryujinx.app` archived 2026-08) |
| **Sudachi** | yuzu | Win, Linux, macOS, Android | Dead: `sudachi.emuplace.app` unreachable, GitHub `emuplace` 404 | 1.0.15, 2025 (U) | none |
| **Torzu** | yuzu | Win, Linux, Android | Maintenance only; primary repo is a Tor hidden service, no official clearnet site | unknown (U) | Codeberg and NotABug mirrors both 404 today |
| **yuzu** (original) | yuzu | Win, Linux, Android | Discontinued 2024-03 (settlement) | mainline 1734 / EA 4176 (U) | none; `yuzu-emu.org` dead |
| **Ryujinx** (original) | Ryujinx | Win, Linux, macOS | Discontinued 2024-10-01 | 1.1.1403 (U) | none; `github.com/Ryujinx/Ryujinx` 404 |
| **Skyline** | own (Android) | Android | Dead; source removed from its GitHub repo at Nintendo's request | 2023 (U) | `github.com/skyline-emu/skyline` is now a notice only |
| **Strato** | Skyline fork | Android | Dead (last push 2024-09), no releases on its repo | none | `github.com/strato-emu/strato` |
| Pomelo | yuzu | iOS | Dormant since 2025-04 | - | `github.com/Pomelo-Emu` (website repo only) |
| Pound, oboromi | own | desktop | Experimental (Switch 1/2 proof of concept), not usable for games | oboromi v0.0.1 pre | `github.com/pound-emu/pound`, `github.com/0xNikilite/oboromi` |

Also seen in front-end definitions but not checked further: **Eden Duo** (`dev.igawa6.edenduo`, dual-screen Eden fork, `github.com/igawa6/eden-duo`), **Benji-SC** (`org.benjisc.android`, Kenji-NX derivative), Ziunx, Nuzu, Uzuy (domain parked), Sumi, Pine (no live home found), and unofficial Eden "Switch 2" forks (eden-nx2, Eglantine, Lichii...) (U, secondary sources only).

Replacement map for dead projects: yuzu / Citron / Sudachi / Torzu -> **Eden**; Ryujinx -> **Ryubing** (desktop), **Kenji-NX** (Android), **MeloNX** (iOS); Skyline / Strato -> **Eden** or **Lemon** on Android.

## 2. Desktop: yuzu family (Eden, Suyu, Citron, Citron Neo, Sudachi, yuzu, Torzu)

All share one design; only the names differ.

### Program files

| Emulator | Windows | Linux | macOS | Source |
|---|---|---|---|---|
| Eden | `eden.exe` (Qt GUI), `eden-cli.exe` (SDL, no GUI). The v0.2.1 zip holds only these two exe files | `Eden-Linux-v<ver>-<arch>-<compiler>.AppImage`; binary `eden`; Flatpak id `dev.eden_emu.eden` | `/Applications/eden.app/Contents/MacOS/eden` (from `Eden-macOS-v<ver>.dmg`) | ZIP; ESDE; Eden release v0.2.1 |
| Suyu v0.1.4 | `suyu.exe`, `suyu-cmd.exe` (zip also carries `vc_redist.x64.exe`, an installer, left unrun) | `suyu-linux-x86_64.AppImage`, tarball | `suyu-macos-arm64.zip` (app name (U)) | ZIP; GitHub release assets |
| Drippu | `drippu-windows-x64.zip` (exe name (U), not downloaded) | `drippu-linux-x86_64.AppImage` | `drippu-macos-arm64.tar.gz` | GitHub release assets |
| Citron 0.6.1 (already on disk) | `citron.exe`, `citron-cmd.exe`, `citron-room.exe` | `citron*.AppImage` (U) | `citron.app` (U) | listing of the existing `Citron\Citron-Windows-Canary-Refresh_0.6.1` folder |
| Citron Neo | `Citron-windows-nightly-<hash>-x64-msvc.zip` (exe name `citron.exe` (U)) | `citron_nightly-<hash>-linux-x86_64.AppImage` | `Citron-macOS-nightly-<hash>.dmg` | asset names on the unconfirmed host |
| Sudachi | `sudachi.exe`, `sudachi-cmd.exe` (U) | | | memory only |
| yuzu | `yuzu.exe`, `yuzu-cmd.exe` (U) | `yuzu*.AppImage` (U) | | memory only |
| Torzu | `yuzu.exe` (Torzu kept yuzu's names) (U) | | | memory only |

### Command line

Eden, from `docs/user/CommandLine.md` (EDEN-SRC), cross-checked with ESDE (`%EMULATOR_EDEN% -f -g %ROM%` on Windows, Linux and macOS):

```
eden.exe -f -g "<game file>"            full screen, start this game
eden.exe -u <index> -f -g "<game>"      -u / --user <number> = profile INDEX (0-based), skips the profile picker
eden.exe -c "<config file>" ...         --config / -c alternate configuration file
eden.exe -i "<input profile name>" ...  --input-profile / -i, player 1 only
eden.exe -qlaunch | -hlaunch | -setup   Qt front-end only: HOME menu applet / homebrew loader / setup applet
```

- A bare path as the only argument also starts the game (drag and drop).
- `src/yuzu/main_window.cpp` (EDEN-SRC): when a game path or `-f` is given, the saved full-screen setting is overridden by the command line (so a path without `-f` starts windowed).
- `eden-cli.exe` takes the same options and shows no launcher interface at all.
- There is no command-line switch for the update prompt: it is the `check_for_updates` setting in `qt-config.ini` (`UISettings::values.check_for_updates`, `main_window.cpp`). Exact ini section (U).
- Suyu v0.1.4: same code base (rebased on Eden per its README); its `docs/user/CommandLine.md` is a shortened copy of Eden's. `-f -g` and `-u` assumed identical (U - not run).
- Citron, Sudachi, yuzu, Torzu: the classic yuzu form `<exe> -f -g "<game>"`, `-u <index>` (U - from memory; the existing NSW-Home launcher already uses it for Citron).

### Data folders

`src/common/fs/fs_paths.h` and `src/common/fs/path_util.cpp` in each tree:

| Emulator | Normal install (Windows) | Linux | Portable switch | Source |
|---|---|---|---|---|
| Eden | `%APPDATA%\eden` | data `~/.local/share/eden`, config `~/.config/eden`, cache `~/.cache/eden` | a folder named `user` beside the exe (Windows); on Linux/macOS `user` in the current directory | EDEN-SRC |
| Suyu v0.1.4 | `%APPDATA%\suyu` | `~/.local/share/suyu` etc. | `user` beside the exe | SUYU-SRC |
| Citron, Citron Neo | `%APPDATA%\citron` (both use the same name) | `~/.local/share/citron` (U) | `user` beside the exe | `archive/citron` on git.ryujinx.app; unconfirmed Neo host |
| Sudachi | `%APPDATA%\sudachi` (U) | | `user` (U) | Eden's legacy-path table names the folder |
| yuzu, Torzu | `%APPDATA%\yuzu` (Torzu shares it) (U) | | `user` (U) | Eden's legacy-path table names the folder |

Inside the data folder (same layout for the whole family; names from Eden's `fs_paths.h`):

| What | Path |
|---|---|
| Keys | `keys\prod.keys`, `keys\title.keys` (`src/core/crypto/key_manager.cpp`) |
| Installed firmware | `nand\system\Contents\registered\` (U - standard yuzu layout, not re-read today) |
| Save data | `nand\user\save\0000000000000000\<profile UUID>\<title id>\` (U - same) |
| Profiles | `nand\system\save\8000000000000010\su\avators\profiles.dat` (U - same) |
| Configuration | `config\qt-config.ini` (INI). Per-game: `config\custom\<title id>.ini`. Input profiles: `config\input\<name>.ini`. `eden-cli` uses `config\sdl2-config.ini` (`src/frontend_common/config.cpp`, `src/qt_common/config/qt_config.h`) |
| Screenshots | `screenshots\` |
| Others | `sdmc\`, `load\` (mods), `shader\` (under cache), `log\eden_log.txt` (Suyu: `suyu_log.txt`, Citron: `citron_log.txt`), `amiibo\`, `play_time\`, `tas\` |

Notes for a launcher:

- Eden registers the data folders of Citron, Sudachi, yuzu, Suyu and Ryujinx as "legacy paths" for its import feature (`path_util.cpp`). It reads them only when the user imports.
- The revived Suyu uses the same `%APPDATA%\suyu` folder as the 2024 Suyu.
- Citron Neo uses the same `%APPDATA%\citron` folder and the same Android package as the original Citron.
- First run: keys must be present or the emulator shows its own missing-keys dialog before any game starts (U - behaviour not exercised). Eden keeps a `first_start` flag in its UI settings (`main_window.cpp`).
- Single instance: no single-instance lock was found in Eden's `main.cpp` / `main_window.cpp`; a second start opens a second process (U - searched, not exhaustively).

## 3. Desktop: Ryujinx family (Ryubing, Kenji-NX, original Ryujinx)

### Program files

| Emulator | Windows | Linux | macOS | Source |
|---|---|---|---|---|
| Ryubing 1.3.3 | `publish\Ryujinx.exe` (the zip has a top folder `publish`) | `ryujinx-<ver>-linux_x64.tar.gz` -> `publish/Ryujinx`; `ryujinx-<ver>-x64.AppImage`; Flatpak `io.github.ryubing.Ryujinx` | `/Applications/Ryujinx.app/Contents/MacOS/Ryujinx` | ZIP; ESDE; release assets |
| Kenji-NX 2.0.5 | `Ryujinx.exe` at the zip root (same file name as Ryujinx and Ryubing) | `kenjinx-<ver>-linux_x64.tar.gz` (U) | (U) | ZIP; release assets |
| Ryujinx 1.1.1403 (already on disk) | `Ryujinx.exe` | `Ryujinx`, older `Ryujinx.Ava` | `Ryujinx.app` | existing folder; ESDE |

The three cannot be told apart by executable name. Tell them apart by folder, or by file version / product name of the exe (U).

### Command line

Ryubing, from `src/Ryujinx/Utilities/CommandLineState.cs` and `src/Ryujinx/Program.cs` (RYU-SRC). ESDE uses plain `%EMULATOR_RYUJINX% %ROM%`.

```
Ryujinx.exe "<game file>"                       start the game (any argument that is not an option is the game path)
Ryujinx.exe -f "<game>"                         -f / --fullscreen
Ryujinx.exe -p "<profile NAME>" -f "<game>"     -p / --profile takes the profile's name, not an index
Ryujinx.exe -r "<data folder>" ...              -r / --root-data-dir: use this folder instead of %APPDATA%\Ryujinx
Ryujinx.exe --hide-updates ...                  do not show the update-available prompt
Ryujinx.exe --no-gui "<game>"                   (also `nogui`) headless mode: game window only, no launcher interface
Ryujinx.exe -i <application id> "<file>"        -i / --application-id: pick one title inside a multi-title file
```

Other options present: `-g/--graphics-backend`, `--backend-threading`, `--bt`, `--pptc`, `-m/--memory-manager-mode`, `--system-region`, `--system-language`, `--docked-mode`, `--handheld-mode`, `--hide-cursor <mode>`, `--software-gui`, `-la/--local-only-amiibo`, `--install-firmware <path>`, `-rdct/--rd-capture-title-format`.

Kenji-NX 2.0.x, from `src/Ryujinx.UI.Common/Helper/CommandLineState.cs` and `src/Ryujinx/Program.cs` (KEN-SRC): `-r`, `-p`, `-f`, `-g`, `--backend-threading`, `-i`, `--docked-mode`, `--handheld-mode`, `--hide-cursor`, `--hide-updates`, `--software-gui`, `--install-firmware`, `-rdct`, plus `-c / --config <file>`; `--no-gui` / `nogui` also present. It lacks Ryubing's `--pptc`, `-m`, `--system-region`, `--system-language`, `-la`.

Original Ryujinx 1.1.1403: `-r`, `-p`, `-f`, `-g`, `--docked-mode`, `--handheld-mode`, `--hide-cursor` and a bare game path (U - from memory; `--hide-updates` and `--no-gui` may be missing there).

### Data folders

`src/Ryujinx.Common/Configuration/AppDataManager.cs` - identical constants in RYU-SRC and KEN-SRC:

| Platform | Normal | Portable |
|---|---|---|
| Windows | `%APPDATA%\Ryujinx` | a folder named `portable` beside `Ryujinx.exe` |
| Linux | `~/.config/Ryujinx` | `portable` beside the binary |
| macOS | `~/Library/Application Support/Ryujinx` | `portable` beside the binary, or beside the `.app` bundle |

**Ryubing, Kenji-NX and the original Ryujinx all use the same `Ryujinx` folder**, so installed side by side they share keys, firmware, saves and `Config.json` unless each gets its own `portable` folder or `-r`.

Inside the data folder:

| What | Path | Source |
|---|---|---|
| Keys | `system\prod.keys` (also read from `%USERPROFILE%\.switch`) | `AppDataManager.cs` (`KeysDir = "system"`, `KeysDirPathUser`) |
| Installed firmware | `bis\system\Contents\registered\` | (U - standard layout; `DefaultNandDir = "bis"` confirmed) |
| Save data | `bis\user\save\<index>\` with metadata in `bis\user\saveMeta\` | (U - same) |
| Profiles | `system\Profiles.json` | (U) |
| Configuration | `Config.json` (JSON). A `Config.json` beside the exe wins over the one in the data folder | `Program.cs` (`localConfigurationPath` checked first), `ReleaseInformation.ConfigName` |
| Per-game configuration | `games\<title id>\Config.json` (Ryubing) | `Program.cs` |
| Screenshots | `screenshots\` inside the data folder | `src/Ryujinx/Systems/AppHost.cs` |
| Others | `games\<title id>\` (caches), `mods\`, `sdcard\`, `profiles\` (input), `Logs\` (portable: inside the data folder; normal on Windows: beside the exe, as the existing `Ryujinx\Logs` shows) | `AppDataManager.cs` |

Notes for a launcher:

- First run: without keys and firmware a command-line start ends in the emulator's own error dialog (U - not exercised).
- Update prompt: `--hide-updates`, or the update-check setting in `Config.json` (key name (U)).
- Single instance: no lock found in `Program.cs` of either fork (U - searched for mutex only).
- Ryubing's stable channel is a year old; `update.ryujinx.app/latest/query?os=win&arch=x64&rc=stable` answers 1.3.3 and `rc=canary` answers 1.3.356.

## 4. Android

Package and activity read from the APKs downloaded today (APK), cross-checked with ESDE and DAIJ.

| Emulator | Package(s) | Launch intent | Source |
|---|---|---|---|
| Eden | `dev.eden.eden_emulator` (standard); `dev.legacy.eden_emulator` (legacy); `com.miHoYo.Yuanshen` ("optimized" / Genshin spoof); nightly adds `.nightly` to each; older nightly `dev.eden.eden_nightly` | component `<package>/org.yuzu.yuzu_emu.activities.EmulationActivity`, action `android.nfc.action.TECH_DISCOVERED` (or `android.intent.action.VIEW`), data = content URI of the game, mime `application/octet-stream` | APK; `build.gradle.kts` (EDEN-SRC); ESDE; DAIJ |
| Suyu v0.1.4 | `org.suyu.suyu_emu`; flavours `org.suyu.suyu_legacy`, `com.miHoYo.Yuanshen` (clashes with Eden's spoof build) | same activity class `org.yuzu.yuzu_emu.activities.EmulationActivity`, same actions and data | APK; SUYU-SRC. DAIJ's Suyu entry (`dev.suyu.suyu_emu.relWithDebInfo`) is the 2024 Suyu and does not match |
| Lemon v1.6.0 | `dev.lemon.lemon_emulator`; `dev.lemon.lemon_emulator.lite`; spoofed `com.antutu.ABenchMark`; also `dev.legacy.lemon_emulator`, `com.miHoYo.Yuanshen` flavours | component `<package>/dev.lemon.lemon_emu.activities.EmulationActivity` (namespace differs from package), action `android.nfc.action.TECH_DISCOVERED` or `VIEW`, data = content URI | APK; its `build.gradle.kts`; DAIJ |
| Kenji-NX 2.1.0-pr.2 | `org.kenjinx.android`; spoof flavour `com.garena.game.codm` | component `org.kenjinx.android/.MainActivity`, action `org.kenjinx.android.LAUNCH_GAME`, string extra `bootPath` = SAF/document URI of the game (falls back to intent data); optional boolean extra `forceNceAndPptc` | APK; `MainActivity.kt` (KEN-SRC); ESDE (`%EXTRA_bootPath%=%ROMSAF%`); DAIJ |
| Citron / Citron Neo | `org.citron.citron_emu` (`.ea` suffix for early access) | `org.citron.citron_emu/org.citron.citron_emu.activities.EmulationActivity`, `TECH_DISCOVERED`, data URI | `build.gradle.kts` in the archive; DAIJ (activity name from DAIJ only) |
| Sudachi | `org.sudachi.sudachi_emu` (`.ea`) | `.../org.sudachi.sudachi_emu.activities.EmulationActivity`, `TECH_DISCOVERED`, data URI | DAIJ only |
| yuzu | `org.yuzu.yuzu_emu` (`.ea`) | `.../org.yuzu.yuzu_emu.activities.EmulationActivity`, `TECH_DISCOVERED`, data URI | DAIJ only |
| Skyline | `skyline.emu` | `skyline.emu/emu.skyline.EmulationActivity`, `android.intent.action.VIEW`, data URI | ESDE; DAIJ |
| Strato | `org.stratoemu.strato` (`.dev` suffix) | `org.stratoemu.strato/org.stratoemu.strato.EmulationActivity`, `VIEW`, data URI | its `app/build.gradle` + manifest; DAIJ |
| Eden Duo / Benji-SC | `dev.igawa6.edenduo` / `org.benjisc.android` | as Eden / as Kenji-NX (activity `org.kenjinx.android.MainActivity`) | DAIJ only |

All EmulationActivity entries are `launchMode="singleTop"` and exported. All downloaded APKs are `arm64-v8a` only. Android data lives under `Android/data/<package>/files/` (U). Eden is also said to be on Google Play; its own download page links only its Forgejo releases, so that was not confirmed.

## 5. iOS

MeloNX (Ryujinx based): `.ipa` only, sideloaded; home `starforgejo.dev/projects/MeloNX`, linked from `melonx.org`. No launch interface for a PC or Android front-end. Pomelo is dormant.

## 6. Not covered, and why

- **Citron Neo**: its GitHub repos are gone (`citron-neo/CI` blocked by DMCA notice 2026-11-12-nintendo-4, `citron-neo/emulator` 404). A Forgejo at `git.mythrax-rs.org/Citron-NEO/emulator` (created 2026-11-07) carries matching releases, but the project's own site does not link it; the only pointer is a third-party issue quoting Discord. Left for the owner to confirm.
- **Citron (original)**: project sites gone. The 0.6.1 build already on disk was left untouched.
- **Drippu**: only automated pre-release builds exist, no stable build.
- **Ryubing canary 1.3.356**: not a stable build (it is a year newer than stable).
- **yuzu, Ryujinx (originals), Sudachi, Torzu, Skyline, Strato**: no official source publishes builds any more (Torzu: Tor only). No third-party archives used.
- **MeloNX**: iOS `.ipa` only, outside the Windows/Android scope.
- **Eden Duo, Benji-SC, Ziunx, Nuzu, Switch 2 forks**: small derivatives, ownership of their download locations not established.
- Look-alike sites seen and avoided: `kenji-nx.com` (a WordPress guide site, not the project), `sudachi.org`, `eggnsemulator.net`, `edenprodkeys.net`, `uzuy-emu.org` (parked).
