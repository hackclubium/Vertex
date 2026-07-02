# Vertex

![Vertex homepage](/src/screenshot.png)

![Release](https://img.shields.io/github/v/release/hackclubium/Vertex?style=flat-square)
![Build](https://img.shields.io/github/actions/workflow/status/hackclubium/Vertex/release.yml?style=flat-square)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20macOS%20%7C%20Linux-blue?style=flat-square)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white)
![Engine](https://img.shields.io/badge/engine-from%20scratch-ff69b4?style=flat-square)
![Stars](https://img.shields.io/github/stars/hackclubium/Vertex?style=flat-square)

**Vertex** is a web browser written from scratch in C++17, with its own HTML parser, DOM,
CSS engine, JavaScript engine, layout engine, and renderer. No Chromium, no WebView2,
no CEF, no QtWebEngine. Everything that makes it a browser is hand-built.

It runs on **Windows, macOS, and Linux** from a single shared engine, with a thin
native shell on each platform.

## What's built from scratch

- **HTML**: tokenizer with 170+ named entities, tree-construction parser with implicit
  element creation, auto-close rules, foster parenting, scope-aware end tags, and
  formatting element adoption (864 lines)
- **CSS**: 90+ properties, cascade with combinators, attribute selectors,
  `:is()`/`:where()`, `@supports`, custom properties (`var()`), media queries,
  `calc()`/`clamp()`/`min()`/`max()`, viewport
  units, `box-sizing`, `object-fit`, flexbox (wrap/shrink/basis/align-self), grid,
  tables, floats, positioning with percentage offsets, `text-align` (incl. justify),
  `text-indent`, `text-decoration`, `vertical-align`, `linear-gradient()`,
  `box-shadow`, `transform` (translate/scale/rotate), `overflow: auto/scroll`,
  `:hover`/`:focus`/`:checked`/`:disabled`/`:enabled`, `:nth-child()`, `:not()`,
  and `~` selectors, and more
- **JavaScript**: lexer, parser, bytecode compiler, VM with 260+ native functions.
  Supports classes, destructuring, template literals, `async`/`await`, optional
  chaining (`?.`), nullish coalescing (`??`), `for...of`, real RegExp via `<regex>`,
  `fetch()`, `getComputedStyle()`, DOM geometry APIs, observer APIs, `addEventListener`
  with event bubbling, and external `<script src>` loading
- **SVG**: rasterizer for inline and external SVG images. Supports rect/circle/ellipse,
  line/polyline/polygon, path commands (`M/L/H/V/C/S/Q/T/A/Z`), text/tspan, nested
  `<svg>`, `<g>`, defs/`<use>`/symbol reuse, linear/radial gradients, viewBox and
  `preserveAspectRatio`, transforms, CSS class/style rules, opacity, stroke dash arrays,
  line caps/joins, fill rules, and all CSS named colors
- **Layout**: block, inline (with line breaking), float, table (auto column sizing),
  flex (row/column, wrap, shrink, basis, align-self, justify-content, gap), grid
  (column tracks), positioned (static/relative/absolute/fixed with % offsets)
- **Rendering**: per-platform painter, DPI-aware, HiDPI scaling
- **Forms**: interactive `<input>`/`<textarea>` with typing, cursor, and GET submission
- **Auto-updater**: checks GitHub releases on startup, downloads in background,
  one-click F12 to apply and restart
- Tabs, per-tab history, zoom, find-in-page, async image loading (6 concurrent)

## Dependencies

Only three things are not hand-written:

- **libcurl**: HTTP/HTTPS and TLS (you don't hand-roll TLS)
- **stb_image**: JPEG/PNG/etc. decoding (vendored single header)
- **OS drawing APIs**: Direct2D + DirectWrite (Windows), Core Graphics + Core Text
  (macOS), GTK3 + Cairo + Pango (Linux). These only plot pixels, glyphs, and bitmaps;
  all layout and paint logic is Vertex's own.

## Build

Requires **CMake 3.20+** and a C++17 compiler. The version is derived automatically
from the latest git tag. libcurl is downloaded via CMake if a system copy isn't found.

### Windows
Visual Studio build tools with the x64 C++ toolchain.

```bat
build.bat
```
```bat
build\Release\Vertex.exe
```

### macOS
Xcode command line tools.

```sh
cmake -B build
cmake --build build
open build/Vertex.app
```

### Linux
GTK3 development headers (and ideally system libcurl):

```sh
sudo apt-get install -y build-essential cmake libgtk-3-dev libcurl4-openssl-dev pkg-config
cmake -B build
cmake --build build
./build/Vertex
```

## Releases

Prebuilt binaries for all three platforms are published on the
[Releases page](https://github.com/hackclubium/Vertex/releases), built by CI on
every tag. Vertex auto-updates: on startup it checks for a newer release and
downloads it in the background. Press **F12** when the status bar shows an
update is ready to apply it instantly.

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| Ctrl+L | Focus address bar |
| Ctrl+T / Ctrl+W | New / close tab |
| Ctrl+R / F5 | Reload |
| Ctrl+F | Find in page |
| Ctrl+G / Ctrl+Shift+G | Find next / previous |
| Ctrl++/- | Zoom in/out |
| Alt+Left/Right | Back / forward |
| F12 | Apply pending update |

## Running engine tests

```bat
build\Release\vertex-tests.exe html
build\Release\vertex-tests.exe css
build\Release\vertex-tests.exe layout
build\Release\vertex-tests.exe paint
build\Release\vertex-tests.exe js
build\Release\vertex-tests.exe network
```

Fixtures live under `tests/fixtures`. Each has an input file and an expected text
snapshot. When engine behavior changes intentionally, change the implementation first,
inspect the failing output, then update the snapshot only once the new behavior is correct.

## Diagnostics

Two offline tools render without the GUI, useful for debugging:

```sh
build/dump_layout page.html [viewportWidth]   # prints the box tree + geometry
build/dump_js script.js                       # runs the JS engine
```
