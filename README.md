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
  <strong>A tiny browser with a real engine inside it.</strong><br>
  HTML, CSS, JavaScript, layout, SVG, painting, navigation, tabs, and native shells,
  built in C++ without Chromium, WebView, CEF, QtWebEngine, or a borrowed browser core.
</p>

---

Vertex is a from-scratch web browser project for people who want to see the browser
as a thing you can open, inspect, change, break, fix, and understand.

It is not a Chromium wrapper wearing a custom toolbar. The interesting parts live in
this repository: the parser, DOM, stylesheet engine, JavaScript runtime, box tree,
layout algorithms, SVG renderer, painting path, resource cache, forms, event bridge,
history, updater, and native application chrome.

The goal is not to pretend the modern web is small. The goal is to make a browser
engine that grows visibly, one compatibility pass at a time.

## Why Does The Code Have AI Comments?

AI comments are included in the code for readibility. I promise you none of this is AI generated code.

## What It Feels Like

Vertex is already a usable experimental browser shell:

| Area | Status |
|---|---|
| Platforms | Windows, macOS, and Linux native shells over one shared engine |
| Pages | Loads real HTTP/HTTPS pages, images, CSS, scripts, SVGs, and local `vertex://` pages |
| UI | Tabs, address bar, profile-backed history/bookmarks/downloads, reload/stop/home, zoom, find-in-page, status text |
| Updates | GitHub release checking, background portable download, and helper-assisted install with `F12` |
| Performance | Cached resources, cached stylesheets, cached selector parsing, dirty layout paths, and hover fast paths |
| Profile | Per-user storage for settings, history, bookmarks, downloads, cookies, local storage, and session restore |
| Testing | Dedicated HTML, CSS, layout, paint, JS, network, and layout-engine suites |

It is still young. Some sites will look strange, some JavaScript will hit missing
APIs, and layout is still being expanded. That is the point: each weird page becomes
a new piece of engine work.

## The Engine

Vertex is split into a portable browser engine plus thin native shells:

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

The platform layer opens windows, receives input, and draws pixels. The engine decides
what the page means, where boxes go, what scripts can touch, and what gets painted.

### Built In This Repo

- HTML tokenizer and parser with entity handling, autoclose behavior, rawtext, and
  browser-style recovery paths.
- CSS cascade with combinators, attributes, pseudo-classes, relational selectors
  like `:has()`, filtered `:nth-child(... of selector)`, media/support queries,
  custom properties, logical properties, transforms, gradients, flex, grid, tables,
  floats, positioning, form styling, and viewport/math functions.
- JavaScript lexer, parser, compiler, VM, native DOM bindings, timers, events,
  promises, async fetch surface, storage, DOM selectors, geometry APIs, and
  observer APIs.
- Layout engine for block, inline, line boxes, floats, tables, flex, grid, replaced
  elements, positioned boxes, scrolling, and dirty-layout invalidation.
- SVG renderer for inline and external SVGs, paths, gradients, transforms, text,
  symbols, `use`, class/style rules, stroke/fill behavior, and raster fallback.
- Paint and hit testing for text, boxes, links, images, controls, SVG, hover, focus,
  dirty regions, and cached rendering paths.

### Not Built From Scratch

Vertex intentionally uses a few low-level dependencies:

| Dependency | Why |
|---|---|
| libcurl | HTTP/HTTPS and TLS |
| stb_image | PNG/JPEG/etc. image decoding |
| Direct2D / DirectWrite | Windows pixels and glyphs |
| Core Graphics / Core Text | macOS pixels and glyphs |
| GTK3 / Cairo / Pango | Linux windowing, pixels, and glyphs |

Those libraries do not supply a browser engine. They do transport, decoding, windows,
text shaping, and drawing. The browser behavior is Vertex.

## Download

Prebuilt releases are published here:

[github.com/hackclubium/Vertex/releases](https://github.com/hackclubium/Vertex/releases)

Release assets are produced by GitHub Actions whenever a tag is pushed:

| Platform | Asset |
|---|---|
| Windows | `Vertex-windows-installer.exe` |
| macOS | `Vertex-macos-installer.dmg` |
| Linux | `Vertex-linux-installer.tar.gz` |

Releases also include updater assets:

| Platform | Updater Asset |
|---|---|
| Windows | `Vertex-windows-portable.exe` |
| macOS | `Vertex-macos-portable` |
| Linux | `Vertex-linux-portable` |

Vertex checks for newer GitHub releases on startup. When an update is ready,
press `F12`; Vertex launches `VertexUpdater`, exits, swaps in the portable
binary, and restarts.

## Profile Data

Vertex creates its profile folders on first launch, whether it came from an
installer, package, or local developer build.

| Platform | Profile | Cache |
|---|---|---|
| Windows | `%LOCALAPPDATA%\Vertex\User Data\Default` | `%LOCALAPPDATA%\Vertex\Cache\Default` |
| macOS | `~/Library/Application Support/Vertex/Default` | `~/Library/Caches/Vertex/Default` |
| Linux | `~/.config/Vertex/Default` | `~/.cache/Vertex/Default` |

The default profile contains simple files such as `history.tsv`, `bookmarks.tsv`,
`downloads.tsv`, `settings.json`, `cookies.tsv`, `local_storage/`, and
`session_restore.json`. The browser also exposes `vertex://settings`,
`vertex://site-data`, `vertex://history`, `vertex://bookmarks`, and
`vertex://downloads`.

## Build

Vertex uses CMake and C++17. The app version is derived from the latest git tag.

### Windows

Requires Visual Studio Build Tools with the x64 C++ toolchain.

```bat
build.bat
build\Release\Vertex.exe
```

### macOS

Requires Xcode command line tools.

```sh
cmake -B build
cmake --build build
open build/Vertex.app
```

### Linux

Requires GTK3 development headers.

```sh
sudo apt-get install -y build-essential cmake libgtk-3-dev libcurl4-openssl-dev pkg-config
cmake -B build
cmake --build build
./build/Vertex
```

## Controls

| Shortcut | Action |
|---|---|
| `Ctrl+L` | Focus the address bar |
| `Ctrl+T` / `Ctrl+W` | New tab / close tab |
| `Ctrl+R` or `F5` | Reload |
| `Ctrl+F` | Find in page |
| `Ctrl+G` / `Ctrl+Shift+G` | Next / previous match |
| `Ctrl++` / `Ctrl+-` | Zoom in / out |
| `Alt+Left` / `Alt+Right` | Back / forward |
| `F12` | Install a downloaded update |

## Test The Engine

The test runner is intentionally split by subsystem so compatibility work can stay
small and measurable.

```bat
build\Release\vertex-tests.exe html
build\Release\vertex-tests.exe css
build\Release\vertex-tests.exe layout
build\Release\vertex-tests.exe paint
build\Release\vertex-tests.exe js
build\Release\vertex-tests.exe network
build\Release\vertex-layout-engine-tests.exe
```

For offline debugging:

```sh
build/dump_layout page.html [viewportWidth]
build/dump_js script.js
```

`dump_layout` is especially useful when a real site looks broken. It prints the box
tree and geometry so layout bugs can be reduced into focused tests instead of guessed
from screenshots.

## Performance Debugging

Set `VERTEX_PERF=1` before launching Vertex to print per-page timing counters while
you browse:

```sh
VERTEX_PERF=1 ./build/Vertex
```

On Windows:

```bat
set VERTEX_PERF=1
build\Release\Vertex.exe
```

The log includes fetch time, resource requests/cache hits, style time, layout time,
paint time, JavaScript parse/run time, and whether layout was reused. It is meant for
real-page work like Wikipedia debugging, where guessing from a screenshot is usually
slower than checking which subsystem is actually hot.

## Compatibility Philosophy

Vertex improves by taking real web failures and turning them into small engine facts:

1. Save or distill the page.
2. Identify whether the failure is HTML, CSS, JS, layout, paint, network, or platform.
3. Add the smallest regression that captures the behavior.
4. Fix the engine.
5. Keep the test forever.

Wikipedia has been the main stress test because it exercises the sort of ordinary
modern-web machinery that small browsers usually miss: ResourceLoader scripts, dense
CSS, logical properties, SVG sprites, form controls, floats, positioned elements,
selectors, events, history, scrolling, and lots of reused resources.

## Project Shape

```text
src/
  css/          stylesheet parsing, cascade, computed style
  html/         tokenizer, parser, embedded resources
  js/           lexer, compiler, VM, runtime, DOM bridge
  layout/       box tree and layout engine
  network/      fetcher, URL handling, cache, text decoding
  paint/        display-list pieces
  platform/     native shells and shared browser chrome
  render/       painting, SVG, images, fonts
  test/         subsystem and regression tests
tools/
  dump_layout   offline layout inspection
  dump_js       offline JS execution
```

## Why Vertex Exists

Browsers are usually treated like impossible magic: too large to touch, too tangled
to learn, too industrial to build for fun.

Vertex is the opposite bet. It says a browser can start small, stay readable, and
still become real by accumulating correct behavior in public. Every feature is an
invitation to understand one more piece of the web.
