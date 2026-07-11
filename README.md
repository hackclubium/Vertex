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

<p align="center">
  <strong>A browser engine built from scratch in C++.</strong><br>
  HTML, CSS, JS, layout, SVG, rendering, tabs, navigation, the whole deal.<br>
  No Chromium. No WebView. No CEF. No QtWebEngine. Just us.
</p>

---

So yeah, this is a web browser where every part is written from scratch. The parser, the DOM, the CSS engine, the JavaScript runtime, the layout engine, the SVG renderer, the network stack, the window shell, everything. It's all in this repo.

If you've ever wondered how browsers actually work under the hood, this is a place where you can poke around and find out. The code has a lot of comments on purpose, because reading other people's browser code without comments is torture.

## What Works

It's early days, but Vertex can load real websites and do a decent amount:

| Area | What's there |
|---|---|
| Platforms | Windows, macOS, Linux, all sharing the same engine |
| Pages | HTTP/HTTPS, images, CSS, JS, SVGs, `<canvas>`, and `vertex://` internal pages |
| Onion sites | `.onion` over Tor, either through the embedded Arti bridge or a local SOCKS proxy |
| UI | Tabs, address bar, history, bookmarks, downloads, reload, zoom, find-in-page, context menus |
| Search | DuckDuckGo for address bar queries |
| Updates | Checks GitHub for new releases, downloads in the background, `F12` to install |
| Speed | Caches resources, stylesheets, selector parsing, dirty layout, hover fast paths, viewport culling |
| Profile | Per-user settings, history, bookmarks, downloads, cookies, local storage, session restore |
| Testing | Test suites for HTML, CSS, layout, paint, JS, network, and codecs |

Some pages still break. Some JS features aren't there yet. Layout is a work in progress. But every broken page turns into something new getting built.

## How It's Set Up

Vertex has one engine that works everywhere, with thin platform-specific shells on top:

```text
          native shell
   Windows / macOS / Linux
              |
              v
        BrowserChrome
 tabs, navigation, URL state, updater
              |
              v
   HTML -> DOM -> CSS cascade -> layout tree -> paint
              |
              v
       JS runtime + DOM bridge
```

The platform layer does windows, input, and pixels. The engine handles everything else: parsing, DOM, styles, layout, scripting, rendering.

### What We Built

- **HTML** -- tokenizer and parser with entity handling, auto-close, rawtext/RCDATA modes, and error recovery that handles real-world mess.
- **CSS** -- cascade with combinators, attributes, pseudo-classes (`:hover`, `:has()`, `:nth-child(... of selector)`), media/supports queries, custom properties, logical properties, transforms, gradients, flex, grid, tables, floats, sticky positioning, form styling, viewport and math functions.
- **JavaScript** -- lexer, parser, compiler, VM, DOM bindings, timers, events, promises, `fetch`, `XMLHttpRequest`, `localStorage`, `sessionStorage`, `cookie`, a hand-rolled WebSocket client (we wrote the handshake, framing, and masking ourselves, curl just handles the encrypted pipe for `wss://`), DOM selectors, geometry APIs, observer APIs, `requestAnimationFrame`, `performance.now()`, `matchMedia`, `navigator.clipboard`, `history.pushState`/`replaceState`/`go()`, `console.*`, messaging (`postMessage`, `MessageChannel`, `BroadcastChannel`), `MutationObserver`, `IntersectionObserver`, `ResizeObserver`.
- **WebAssembly** -- MVP hand-rolled parser/interpreter for small still-page modules: `WebAssembly.Module`, `Instance`, `instantiate`, `validate`, exported functions, i32 arithmetic/comparisons, internal calls, linear memory, `i32.load`/`i32.store`, and minimal `ArrayBuffer`/`Uint8Array` byte input.
- **Canvas 2D** -- full `<canvas>` context with `fillRect`, `strokeRect`, `clearRect`, paths, arcs, bezier/quadratic curves, `fill`, `stroke`, `save`/`restore`, transforms, `drawImage`.
- **Layout** -- block, inline, line boxes, floats, tables, flex, grid, replaced elements, positioned boxes (relative, absolute, sticky, fixed), scrolling, overflow containers, dirty-layout invalidation, adaptive hover throttling.
- **SVG** -- inline and external SVGs, paths, gradients, transforms, text, symbols, `<use>`, class/style rules, stroke/fill, raster fallback.
- **Painting** -- text, boxes, links, images, controls, SVG, canvas, hover, focus, dirty regions, cached render paths, hit testing, spatial index for hover.
- **Fonts** -- TrueType parsing, `@font-face` web font loading on all platforms.
- **Codecs** -- hand-rolled PNG decoder (all color types, tRNS), JPEG decoder (Huffman, IDCT, progressive), WebP still-image decoder (VP8, VP8L, VP8X alpha), DEFLATE/inflate (RFC 1951), CRC-32.

### Zero Third-Party Dependencies

The only external code Vertex uses is platform-native APIs for drawing and text:

| Dependency | What it does |
|---|---|
| Direct2D / DirectWrite | Windows pixels and glyphs |
| Core Graphics / Core Text | macOS pixels and glyphs |

Linux doesn't use any of those. Windowing (XCB), 2D rendering, text, `<canvas>`, TrueType fonts, PNG/JPEG/WebP decoding, DEFLATE, WebSocket, TLS, and the HTTP client are all hand-rolled. No GTK, no Cairo, no Pango, no fontconfig, no stb_image, no libcurl.

Everything is built from scratch in this repo.

One exception worth calling out: `.onion` support can use Arti, the Tor Project's Rust Tor implementation. The browser still does its own URL parsing, HTTP, TLS plumbing, and SOCKS fallback, but Tor itself is not something we want to fake badly in browser code. If the Arti bridge is built, Vertex loads it next to the app. If not, it falls back to a local Tor/Arti SOCKS proxy.

## Download

Grab a release: [github.com/hackclubium/Vertex/releases](https://github.com/hackclubium/Vertex/releases)

| Platform | Installer |
|---|---|
| Windows | `Vertex-windows-installer.exe` |
| macOS | `Vertex-macos-installer.dmg` |
| Linux | `Vertex-linux-installer.tar.gz` |

Each release also has portable updater binaries. Vertex checks for new releases on startup, and you can press `F12` to install one. It launches `VertexUpdater`, swaps the binary, and restarts.

Releases built with `-DVERTEX_BUILD_ARTI=ON` include the Arti bridge (`vertex_arti.dll`, `libvertex_arti.dylib`, or `libvertex_arti.so`). That means `.onion` URLs can work without you starting `tor` in a terminal first. If the bridge is missing, Vertex still supports `.onion` through a SOCKS proxy at `127.0.0.1:9050`, or whatever you put in `VERTEX_TOR_SOCKS`.

## Where Profile Data Lives

On first launch, Vertex creates profile and cache folders:

| Platform | Profile | Cache |
|---|---|---|
| Windows | `%LOCALAPPDATA%\Vertex\User Data\Default` | `%LOCALAPPDATA%\Vertex\Cache\Default` |
| macOS | `~/Library/Application Support/Vertex/Default` | `~/Library/Caches/Vertex/Default` |
| Linux | `~/.config/Vertex/Default` | `~/.cache/Vertex/Default` |

Profile stuff includes `history.tsv`, `bookmarks.tsv`, `downloads.tsv`, `settings.json`, `cookies.tsv`, `local_storage/`, and `session_restore.json`. You can see all of it from inside the browser at `vertex://settings`, `vertex://site-data`, `vertex://history`, `vertex://bookmarks`, and `vertex://downloads`.

## Building

CMake and C++17. Version comes from the latest git tag.

Rust is optional. By default, the browser builds without downloading Rust crates, and `.onion` uses the SOCKS fallback. If you want the embedded Arti bridge, opt in with `-DVERTEX_BUILD_ARTI=ON`.

### Windows

Needs Visual Studio Build Tools with the x64 C++ toolchain.

```bat
build.bat
build\Release\Vertex.exe
```

### macOS

Needs Xcode command line tools.

```sh
cmake -B build
cmake --build build
open build/Vertex.app
```

### Linux

Only needs XCB development headers. No GTK, Cairo, Pango, or fontconfig. Vertex does its own windowing, rendering, text, fonts, and `<canvas>` on Linux.

```sh
sudo apt-get install -y build-essential cmake libxcb1-dev pkg-config
cmake -B build
cmake --build build
./build/Vertex
```

### Optional Arti bridge

If you want `.onion` support without running a separate Tor process, install Rust and build the `vertex_arti` target once:

```sh
rustup default stable
cmake -B build -DVERTEX_BUILD_ARTI=ON
cmake --build build --target vertex_arti
cmake --build build
```

On Windows with Visual Studio builds:

```bat
cmake -S . -B build -DVERTEX_BUILD_ARTI=ON
cmake --build build --config Release --target vertex_arti
cmake --build build --config Release
```

No Rust? Still fine. Run Tor or Arti yourself and Vertex will use the SOCKS proxy:

```sh
VERTEX_TOR_SOCKS=127.0.0.1:9050 ./build/Vertex
```

On Windows PowerShell:

```powershell
$env:VERTEX_TOR_SOCKS="127.0.0.1:9050"
.\build\Release\Vertex.exe
```

If you built the Arti library somewhere else, point Vertex at it:

```sh
VERTEX_ARTI_LIB=/path/to/libvertex_arti.so ./build/Vertex
```

## Keyboard Shortcuts

| Shortcut | What it does |
|---|---|
| `Ctrl+L` | Focus the address bar |
| `Ctrl+T` / `Ctrl+W` | New tab / close tab |
| `Ctrl+R` or `F5` | Reload |
| `Escape` | Stop loading |
| `Ctrl+F` | Find in page |
| `Ctrl+G` / `Ctrl+Shift+G` | Next / previous match |
| `Ctrl++` / `Ctrl+-` | Zoom in / out |
| `Ctrl+0` | Reset zoom |
| `Alt+Left` / `Alt+Right` | Back / forward |
| `Ctrl+1`-`Ctrl+9` | Switch to tab by number |
| `Ctrl+H` / `Ctrl+B` / `Ctrl+J` | History / bookmarks / downloads |
| `F12` | Install a downloaded update |

## Tests

Tests are organized by what they cover:

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

There are also some offline debugging tools:

```sh
build/dump_layout page.html [viewportWidth]
build/dump_js script.js
build/render_probe page.html [viewportWidth]
build/test_url
```

- `dump_layout` prints the box tree and geometry, handy when a site is broken and you'd rather look at structured output than stare at screenshots.
- `render_probe` runs the full layout/paint pipeline on a fixture and outputs metrics, box tree, and paint order.
- `test_url` runs unit tests for URL helpers.

## Performance Debugging

Set `VERTEX_PERF=1` before launching to get per-page timing info:

```sh
VERTEX_PERF=1 ./build/Vertex
```

On Windows:

```bat
set VERTEX_PERF=1
build\Release\Vertex.exe
```

The output includes fetch time, resource requests and cache hits, style time, layout time, paint time, JS parse/run time, and whether layout was reused.

## Fixing the Web, One Page at a Time

This is how Vertex grows:

1. Find a page that breaks.
2. Figure out which part is failing, HTML, CSS, JS, layout, paint, network, or platform.
3. Write the smallest test you can that reproduces the bug.
4. Fix it.
5. Keep the test.

Wikipedia has been the go-to test site because it hits so many things small browsers usually skip: ResourceLoader scripts, dense CSS, logical properties, SVG sprites, form controls, floats, positioned elements, selectors, events, history, scrolling, and cached resources.

## Project Layout

```text
src/
  codec/         PNG, JPEG, WebP, DEFLATE decoders, CRC-32
  css/           stylesheet parsing, cascade, computed style
  font/          TrueType parsing, @font-face loading
  html/          tokenizer, parser, embedded resources
  js/            lexer, compiler, VM, runtime, DOM bridge, Canvas 2D
  layout/        box tree and layout engine
  network/       fetcher, URL handling, cache, text decoding, TLS, WebSocket
  paint/         display-list pieces
  platform/      native shells and shared browser chrome
  render/        painting, SVG, images, fonts
tools/
  dump_layout    offline layout inspection
  dump_js        offline JS execution
  render_probe   deterministic layout/paint diagnostics
  test_url       URL helper unit tests
tests/           subsystem and regression tests
```

## Why This Exists

Browsers feel like these giant, impenetrable black boxes that nobody can really understand.

Vertex is trying to be the opposite. A browser that starts small, stays readable, and gets more capable over time by actually shipping correct behavior. Every feature is a chance to understand one more piece of how the web works.
