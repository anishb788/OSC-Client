# OpenShop

A modern homebrew browser for the Wii, inspired by the Wii Shop Channel's look and feel.
It's an **Open Shop Channel client**: the app catalog comes from the
[Open Shop Channel](https://oscwii.org) repository.

Status: **Milestone 2 (live catalog)**. OpenShop loads the real catalog from the Open Shop Channel,
shows real app icons, caches both on the SD card and keeps working offline from that cache.
Downloading and installing apps isn't implemented yet (Milestone 3).

The catalog is fetched over **plain HTTP** (`http://hbb1.oscwii.org/api/v4`). The Wii build has no TLS
yet (planned for Milestone 4), so the catalog and icons travel unencrypted and unauthenticated. Don't
use OpenShop on a network you don't trust to carry that. Every request identifies itself with the
User-Agent `OpenShop/0.2.0 (Nintendo Wii; Open Shop Channel client)` (`Linux;` on PC).

## Features so far

- Shop-style UI: white surfaces, light-blue accents, rounded pill buttons, fade-through-white
  transitions, hover sounds, Wii Remote pointer cursor. All artwork and sounds are drawn or
  synthesized in code. No Nintendo assets are used.
- **Live catalog** at startup, cached on the SD card (1 hour before it is fetched again). Without
  network, the cached catalog is shown with an "Offline" indicator and its date; without a cache either,
  an error screen offers Retry and Exit.
- **Refresh** button in the top bar: goes back to a loading screen, re-fetches the catalog and lands on
  Home. If it fails, the previous catalog is kept and a "Couldn't refresh" message gives the reason.
  Press B to cancel. A Refresh within 60 s of the last successful fetch only rebuilds Home.
- Real 128x48 icons, loaded as they come into view and cached on the SD card.
- Home: featured app, category tiles, Popular and Newest.
- Paged app lists with Popular / Newest / A–Z sorting.
- App details: version, release date, downloads, supported consoles and controllers,
  warnings for apps that modify NAND, and a scrollable description.
- **Search** across name, author, slug and description, using an on-screen keyboard or a USB keyboard.
- **Custom background music**: put `bgm.mp3` (or `bgm.ogg`) in the app folder
  (`sd:/apps/OpenShop/bgm.mp3`). Toggle it with the 1 button (F2 on PC).
- 4:3 and 16:9 layouts, following the console's widescreen setting.

## Controls

| Wii Remote / Classic | PC build | Action |
|---|---|---|
| Point + A | Mouse + left click | Select |
| D-pad | Arrow keys | Move focus |
| B | Esc / right click | Back |
| + / − | PageDown / PageUp | Next / previous page, scroll the description |
| HOME | F1 | Exit menu |
| 1 | F2 | Toggle music |

## Development

```
./dev watch              # Wii build in Dolphin, rebuilt and relaunched on every save
./dev watch-pc           # native Linux build, the fastest way to iterate on UI
./dev run --screen detail   # start on a screen: home, search, list, detail, dialog
./dev pc --mock --offline --refresh   # dev options (see the header of ./dev)
./dev package            # dist/OpenShop.zip for the Homebrew Channel
./dev build              # Wii build only (add WERROR=1 to fail on warnings, DEBUGTOOLS=1 for the hardware test build)
./dev wiiload            # send the .dol to a real Wii running HBC (WIILOAD=tcp:<wii-ip>)
./dev clean | headers | shell
```

### Dev options

`run`, `watch`, `pc` and `watch-pc` take these before any other argument (the header of `./dev` is the
reference):

| Option | Effect |
|---|---|
| `--screen <home\|search\|list\|detail\|dialog>` | open that screen after the startup load (a Refresh always ends on Home) |
| `--mock` | use the compiled-in mock catalog: no network, no SD cache, placeholder icons |
| `--offline` | use the SD cache only, no network |
| `--refresh` | ignore the cache TTL and fetch now |
| `--fail[=startup\|refresh\|both\|none]` | make the startup load fail (error screen, Retry works) and/or every Refresh fail ("Couldn't refresh"); plain `--fail` = startup |
| `--topbar all` | preview every top-bar button (Downloads, Refresh, Settings, with a fake progress strip and badge) |
| `--mockdelay <ms>` | how long the mock load takes (default 300) |
| `--ratelimit <s>` | Refresh rate limit (default 60, 0 = off) |
| `--cachettl <s>` | catalog cache TTL instead of 1 h (0 = always stale) |
| `--api <url>` | catalog API base instead of `http://hbb1.oscwii.org/api/v4` |
| `--icons <url>` | fetch icons from `<url>/<slug>/icon.png` (a local test server) |
| `--autorefresh <ms>` | press Refresh by itself that long after Home appears |
| `--perf` | log draw / present / frame times and the texture count |
| `--nettest <script>`, `--nettestbg <url>`, `--nettestquit` | dev-only network self-test (syntax in `src/net/NetSelfTest.h`) |

On PC they reach the app as `OPENSHOP_*` environment variables (`OPENSHOP_MOCK`, `OPENSHOP_API`, ...). On
the Wii build, which can't see environment variables, `./dev` writes them as `key=value` lines to
`sd:/apps/OpenShop/devstart` (`screen=detail`, `mock=1`, ...) before launching Dolphin. A real Wii reads
the same file, so **delete `devstart`** from the SD card when you're done testing, or the app keeps using those options.

### Testing

- **Host tests:** `make -f Makefile.pc test` builds and runs every test (HTTP parsing and transfer, the
  worker, OSC API parsing, the catalog cache, icons, layouts, search). Add `WERROR=1` to fail on warnings and
  `SAN=1` for AddressSanitizer and UBSan. The network tests talk to a loopback server inside the test
  binaries, so no internet access is needed.
- **Test server:** `tools/http_test_server.py` (stdlib Python) serves the endpoints the network layer is
  checked against: fixed and chunked bodies, redirects (including to https), a stalled response, junk JSON,
  a slow download, and `/headers`, which echoes the request head so the User-Agent can be checked. Point the app at it
  with `./dev pc --api http://127.0.0.1:<port>/api/v4` (catalog requests then reach its `/headers`-style logging); it logs every request head to stderr.
- **Debug build for real hardware:** `./dev build DEBUGTOOLS=1` (run `./dev clean` when switching) builds an
  on-screen network test bench. Every log line goes to the screen and to
  `sd:/apps/OpenShop/debuglog.txt`, and per-test logs next to it. Don't ship this build.
- **Dolphin networking:** Dolphin emulates the Wii's network stack, so the app's libogc `net_*` code runs
  as it does on a console, using the host's connection. To test the offline paths, use `--offline` or
  `--fail`, or disconnect the host from the network; to test the error screen with no cache, move
  `sdcard/apps/OpenShop/cache/` away first. Dolphin only writes the virtual SD back on a clean shutdown (see
  below), so the cache written during a session is there after `./dev` stops it. Real-Wii results
  (load time, Wi-Fi behaviour) can't be reproduced in Dolphin.

- **Toolchain:** the official `devkitpro/devkitppc` container via podman. A native devkitPro
  install is used instead when `$DEVKITPPC` is set.
- **PC build:** needs `sudo apt install libsdl2-dev libsdl2-ttf-dev libsdl2-mixer-dev libsdl2-image-dev libjansson-dev zlib1g-dev`
  (plus `python3` for the test server). The Wii build needs nothing beyond the `devkitpro/devkitppc` image: SDL2,
  SDL2_ttf, SDL2_mixer, SDL2_image, jansson, libpng and zlib are already in it.
- **Dolphin SD card:** Dolphin's virtual SD card is synced with `./sdcard`, which is
  git-ignored. The app reads `sdcard/apps/OpenShop/` both in Dolphin and in the PC build.
  Dolphin only writes the virtual SD back to `./sdcard` when emulation shuts down cleanly,
  so `./dev` stops it with one SIGINT and waits (up to `DOLPHIN_STOP_TIMEOUT`, default 10 s)
  instead of killing it; a forced kill prints a warning that the session's SD writes are lost.
  The real folder is `.dolphin/sdcard` and `./sdcard` is a symlink to it (`./dev` sets this up
  and moves an old real `./sdcard` over). The Flatpak gets write access to `.dolphin/` only,
  because the sync renames the sync folder, which needs a writable parent.
- **Checks:** `./dev build WERROR=1` / `make -f Makefile.pc WERROR=1` turn warnings into errors.
  `make -f Makefile.pc SAN=1 test` builds the tests (and plain `SAN=1` the app, in `build-pc-san/`)
  with AddressSanitizer and UBSan.
- **Logging:** `LOG(...)` from `src/core/Log.h` prints to the terminal running `./dev`.
- **Editor setup:** run `./dev headers` once for VS Code IntelliSense.

### SD card layout

Everything lives in `sd:/apps/OpenShop/` (`sdcard/apps/OpenShop/` in Dolphin and on PC; `OPENSHOP_DATA` overrides it on PC):

```
boot.dol, meta.xml, icon.png   the Homebrew Channel package (./dev package)
THIRD_PARTY_NOTICES.txt, OFL.txt   licence texts for the linked libraries and the font
bgm.mp3 or bgm.ogg             optional background music (you provide it)
cache/
  cache.json                   manifest: format, base URL, fetch time (the commit point of a cache write)
  contents.<n>.json, information.<n>.json, featured.<n>.json   the catalog as the API sent it
  icons/<slug>.png             downloaded app icons
devstart                       dev options for the Wii build (dev only, delete after testing)
debuglog.txt, nettest.txt      only written by the DEBUGTOOLS / --nettest builds
```

A normal launch writes only inside `cache/`. A corrupt or truncated cache is detected, ignored and replaced.
Deleting `cache/` is always safe.

### Layout

```
src/
  App.*            screen stack, transitions, dialogs, main loop
  core/            shared types, input, logging
  platform/        Wii (WPAD/IR, SD paths) vs. PC (SDL mouse/keyboard)
  gfx/             renderer helpers (AA rounded rects, SDF icons, cursor), text cache, theme
  ui/              immediate-mode UI: focus, pointer + D-pad spatial navigation, widgets
  screens/         Loading, Home, List, Detail, Search
  catalog/         app model, search/sort, OSC API parsing, SD cache, loaders, mock data (tools/gen_mock_catalog.py)
  net/             HTTP client (Wii and POSIX sockets), the one cooperative network worker, test hooks
  audio/           SDL_mixer: synthesized UI sounds, user background music
assets/fonts/      M PLUS Rounded 1c, Latin subset (SIL OFL 1.1, see OFL.txt)
```

## Roadmap

2. **Live catalog (done):** `hbb1.oscwii.org/api/v4`, real icons, offline cache, Refresh.
3. **Installing:** download queue, MD5 check, extract to SD or USB, detect installed apps and updates, uninstall.
4. **Polish:** HTTPS, settings, GameCube controller support.

## Credits

App catalog and metadata: the Open Shop Channel and the Open Shop Channel Team.
Font: M PLUS Rounded 1c by the M+ FONTS Project (SIL Open Font License 1.1, `assets/fonts/OFL.txt`).

OpenShop isn't affiliated with or endorsed by the Open Shop Channel. The Wii build statically links SDL2,
SDL2_ttf, SDL2_image, SDL2_mixer, FreeType, HarfBuzz, libpng, zlib, bzip2, Brotli, libogg, Opus, opusfile,
libmodplug, jansson and libogc (with libfat, wiiuse, bte, wiikeyboard and aesnd). Their licence texts are in
`THIRD_PARTY_NOTICES.txt`, which `./dev package` copies next to `boot.dol`.

Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved.
