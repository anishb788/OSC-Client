# OpenShop

A modern homebrew browser for the Wii, inspired by the Wii Shop Channel's look and feel.
It's an **Open Shop Channel client**: the app catalog comes from the
[Open Shop Channel](https://oscwii.org) repository.

Status: **Milestone 1 (UI foundation)**. All screens work on mock data taken from a snapshot of
the real catalog. Downloading and installing apps isn't implemented yet.

## Features so far

- Shop-style UI: white surfaces, light-blue accents, rounded pill buttons, fade-through-white
  transitions, hover sounds, Wii Remote pointer cursor. All artwork and sounds are drawn or
  synthesized in code. No Nintendo assets are used.
- Home: featured app, category tiles, Popular and Newest.
- Paged app lists with Popular / Newest / A–Z sorting.
- App details: version, release date, downloads, supported consoles and controllers,
  warnings for apps that modify NAND, and a scrollable description.
- **Search** across name, author, slug and description, using an on-screen keyboard or a USB keyboard.
- **Custom background music**: put `bgm.mp3` (or `bgm.ogg`) in the app folder
  (`sd:/apps/OpenShop/bgm.mp3`). Toggle it with the Music button or the 1 button.
- 4:3 and 16:9 layouts, following the console's widescreen setting.

## Controls

| Wii Remote / Classic | PC build | Action |
|---|---|---|
| Point + A | Mouse + left click | Select |
| D-pad | Arrow keys | Move focus |
| B | Esc / right click | Back (on Search: delete a letter) |
| + / − | PageDown / PageUp | Next / previous page, scroll the description |
| HOME | F1 | Exit menu |
| 1 | F2 | Toggle music |

## Development

```
./dev watch              # Wii build in Dolphin, rebuilt and relaunched on every save
./dev watch-pc           # native Linux build, the fastest way to iterate on UI
./dev run --screen detail   # start on a screen: home, search, list, detail, dialog
./dev package            # dist/OpenShop.zip for the Homebrew Channel
```

- **Toolchain:** the official `devkitpro/devkitppc` container via podman. A native devkitPro
  install is used instead when `$DEVKITPPC` is set.
- **PC build:** needs `sudo apt install libsdl2-dev libsdl2-ttf-dev libsdl2-mixer-dev`.
- **Dolphin SD card:** Dolphin's virtual SD card is synced with `./sdcard`, which is
  git-ignored. The app reads `sdcard/apps/OpenShop/` both in Dolphin and in the PC build.
- **Logging:** `LOG(...)` from `src/core/Log.h` prints to the terminal running `./dev`.
- **Editor setup:** run `./dev headers` once for VS Code IntelliSense.

### Layout

```
src/
  App.*            screen stack, transitions, dialogs, main loop
  core/            shared types, input, logging
  platform/        Wii (WPAD/IR, SD paths) vs. PC (SDL mouse/keyboard)
  gfx/             renderer helpers (AA rounded rects, SDF icons, cursor), text cache, theme
  ui/              immediate-mode UI: focus, pointer + D-pad spatial navigation, widgets
  screens/         Home, List, Detail, Search
  catalog/         app model, search/sort, mock data (tools/gen_mock_catalog.py)
  audio/           SDL_mixer: synthesized UI sounds, user background music
assets/fonts/      M PLUS Rounded 1c, Latin subset (SIL OFL 1.1, see OFL.txt)
```

## Roadmap

2. **Live catalog:** fetch `hbb1.oscwii.org/api/v4/contents`, real icons, offline cache.
3. **Installing:** download queue, MD5 check, extract to SD or USB, detect installed apps and updates, uninstall.
4. **Polish:** HTTPS, settings, GameCube controller support.

## Credits

App catalog and metadata: the Open Shop Channel and the Open Shop Channel Team.
Font: M PLUS Rounded 1c by the M+ FONTS Project (SIL Open Font License 1.1).
