# Vertex

<p align="center">
  <img src="src/screenshot.png" alt="Vertex browser screenshot" width="860">
</p>

<p align="center">
  <a href="https://github.com/hackclubium/Vertex/releases"><img alt="Release" src="https://img.shields.io/github/v/release/hackclubium/Vertex?style=for-the-badge"></a>
  <a href="https://github.com/hackclubium/Vertex/actions/workflows/release.yml"><img alt="Build" src="https://img.shields.io/github/actions/workflow/status/hackclubium/Vertex/release.yml?style=for-the-badge"></a>
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-1f6feb?style=for-the-badge&logo=cplusplus&logoColor=white">
  <img alt="Engine" src="https://img.shields.io/badge/engine-from%20scratch-111827?style=for-the-badge">
</p>

Vertex is a browser engine written from scratch in C++.

Not Chromium. Not WebView. Not CEF. Not QtWebEngine. It has its own HTML parser, CSS engine, JS runtime, layout engine, renderer, networking, native shells, and a pile of weird browser stuff that probably should have scared us off by now.

It loads real sites. It also breaks on real sites. Both are kind of the point.

## What Works

| Area | Status |
|---|---|
| Platforms | Windows, macOS, Linux |
| Pages | HTTP, HTTPS, images, CSS, JS, SVG, canvas, internal `vertex://` pages |
| Onion sites | `.onion` through embedded Arti in release builds, or local Tor SOCKS fallback |
| UI | Tabs, address bar, history, bookmarks, downloads, reload, zoom, find, context menus |
| Search | DuckDuckGo from the address bar |
| Updates | Checks GitHub releases, downloads update, `F12` installs it |
| Profile | History, bookmarks, downloads, cookies, local storage, session restore |
| Tests | HTML, CSS, layout, paint, JS, network, codec tests |

Some stuff is real. Some stuff is a stub with enough shape to unblock websites. Some stuff is cursed but useful. Welcome to browsers.

## Fun Internal Pages

These work from the address bar:

```text
vertex://home
vertex://history
vertex://bookmarks
vertex://downloads
vertex://settings
vertex://site-data
vertex://platform-features
```

`vertex://platform-features` is the messy debug page for platform APIs: fullscreen, pointer lock, PiP, file picker, clipboard, notifications, gamepads, permissions, sensors, wake lock, and other browser-shaped things.

## Onion Support

Release builds try to ship the Arti bridge next to Vertex:

```text
vertex_arti.dll
libvertex_arti.dylib
libvertex_arti.so
```

If that file is there, Vertex can use Arti directly for `.onion` URLs. No separate `tor` terminal needed.

If it is not there, Vertex falls back to SOCKS5:

```text
127.0.0.1:9050
```

Override it if needed:

```sh
VERTEX_TOR_SOCKS=127.0.0.1:9050 ./build/Vertex
```

Windows PowerShell:

```powershell
$env:VERTEX_TOR_SOCKS="127.0.0.1:9050"
.\build\Release\Vertex.exe
```

Custom Arti bridge path:

```sh
VERTEX_ARTI_LIB=/path/to/libvertex_arti.so ./build/Vertex
```

Important bit: Vertex does not try to reimplement Tor crypto in C++. That would be a fun way to accidentally build a privacy hazard. It uses Arti for Tor, while Vertex still owns the browser/network integration around it.

## Download

Grab releases here:

```text
https://github.com/hackclubium/Vertex/releases
```

Release assets usually include:

| Platform | Main download |
|---|---|
| Windows | `Vertex-windows-installer.exe` |
| macOS | `Vertex-macos-installer.dmg` |
| Linux | `Vertex-linux-installer.tar.gz` |

Portable bundles are there too. The auto-updater still uses the single-binary portable assets, and separately updates the Arti bridge when the release has one.

## Building

You need CMake and a C++17 compiler.

Rust is optional for local builds. If you do not build the Arti bridge, `.onion` still works through `VERTEX_TOR_SOCKS`.

### Windows

Visual Studio Build Tools, x64 C++ toolchain:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
build\Release\Vertex.exe
```

Build embedded Arti too:

```bat
cmake -S . -B build -A x64 -DVERTEX_BUILD_ARTI=ON
cmake --build build --config Release --target vertex_arti
cmake --build build --config Release
```

### macOS

Xcode command line tools:

```sh
cmake -B build
cmake --build build
open build/Vertex.app
```

With embedded Arti:

```sh
cmake -B build -DVERTEX_BUILD_ARTI=ON
cmake --build build --target vertex_arti
cmake --build build
```

### Linux

XCB headers only. No GTK/Cairo/Pango/fontconfig stack.

```sh
sudo apt-get install -y build-essential cmake libxcb1-dev pkg-config
cmake -B build
cmake --build build
./build/Vertex
```

With embedded Arti:

```sh
cmake -B build -DVERTEX_BUILD_ARTI=ON
cmake --build build --target vertex_arti
cmake --build build
```

## Keyboard Stuff

| Shortcut | Action |
|---|---|
| `Ctrl+L` | Address bar |
| `Ctrl+T` / `Ctrl+W` | New tab / close tab |
| `Ctrl+R` or `F5` | Reload |
| `Escape` | Stop loading / dismiss stuff |
| `Ctrl+F` | Find |
| `Ctrl+G` / `Ctrl+Shift+G` | Next / previous match |
| `Ctrl++` / `Ctrl+-` / `Ctrl+0` | Zoom |
| `Alt+Left` / `Alt+Right` | Back / forward |
| `Ctrl+1`-`Ctrl+9` | Switch tabs |
| `Ctrl+H` / `Ctrl+B` / `Ctrl+J` | History / bookmarks / downloads |
| `F11` | Fullscreen on supported platform shells |
| `F12` | Install downloaded update |

Linux platform feature shortcuts:

```text
Ctrl+Shift+M  open vertex://platform-features
Ctrl+Shift+U  toggle/fill platform feature debug state
```

macOS equivalents:

```text
Cmd+Shift+M
Cmd+Shift+U
```

## Tests

```bat
build\Release\vertex-tests.exe html
build\Release\vertex-tests.exe css
build\Release\vertex-tests.exe layout
build\Release\vertex-tests.exe paint
build\Release\vertex-tests.exe js
build\Release\vertex-tests.exe network
build\Release\vertex-tests.exe codec
build\Release\vertex-layout-engine-tests.exe
```

Useful debug tools:

```sh
build/dump_layout page.html [viewportWidth]
build/dump_js script.js
build/render_probe page.html [viewportWidth]
build/test_url
```

## Perf Debugging

```sh
VERTEX_PERF=1 ./build/Vertex
```

Windows:

```bat
set VERTEX_PERF=1
build\Release\Vertex.exe
```

You get fetch/cache/style/layout/paint/JS timings in the status bar.

## Where Data Goes

| Platform | Profile | Cache |
|---|---|---|
| Windows | `%LOCALAPPDATA%\Vertex\User Data\Default` | `%LOCALAPPDATA%\Vertex\Cache\Default` |
| macOS | `~/Library/Application Support/Vertex/Default` | `~/Library/Caches/Vertex/Default` |
| Linux | `~/.config/Vertex/Default` | `~/.cache/Vertex/Default` |

Mostly TSV and JSON files. Easy to inspect. Easy to delete when something gets weird.

## Project Map

```text
src/
  codec/       PNG, JPEG, WebP, DEFLATE
  css/         parser, cascade, computed style
  font/        TrueType and web fonts
  html/        tokenizer, parser, resources
  js/          lexer, compiler, VM, DOM bridge
  layout/      layout tree and geometry
  network/     HTTP, TLS, WebSocket, cookies, Tor/Arti bridge loader
  paint/       display-list pieces
  platform/    Windows/macOS/Linux shells and browser chrome
  render/      painting, SVG, images, canvas
tests/         subsystem tests
tools/         debug tools
third_party/   optional Arti bridge crate
```

## The Vibe

Browsers are usually giant black boxes. Vertex is trying to be the opposite: small enough to read, weird enough to learn from, and real enough to load pages that were absolutely not designed for it.

The loop is simple:

1. Open a page.
2. Watch it break.
3. Figure out which browser subsystem lied.
4. Fix the smallest thing.
5. Keep the test.

That's basically the whole project.
