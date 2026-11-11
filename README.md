# VPX WebView plugin

> [!WARNING]
> **This is not an official Visual Pinball project.** It is a personal
> experiment for my own homebrew pinball machine. It is not part of
> Visual Pinball X, not supported by the VPX team, and may change or
> disappear at any time. It is in no way meant as a replacement for
> PinUP Player (PUP), FlexDMD, or the Godot-based tooling used with Visual
> Pinball. It will never be an official plugin either: it is built on CEF
> (Chromium), which does not run on iOS or Android, while VPX plugins need
> to work on every platform VPX supports. Use it at your own risk.

A web runtime for Visual Pinball X. Tables open web pages (HTML, CSS,
Canvas, WebGL2, WebGPU, `<video>`, WebAudio). The plugin renders them offscreen
with Chromium, embedded in-process through CEF, and wires the output back into
VPX:

- **picture** through virtual screens: pages are layered onto screens, and a
  screen is shown by flashers in *Display* (or *DMD*) mode (image source link
  `ctrl://webview/display?id=N`) and/or on the backglass, score view or topper
  window
- **sound** into VPX's mixer: everything a page plays is delivered by Chromium
  as PCM and streamed into VPX as a plugin audio source with its own gain.
  Pages are muted locally, so only VPX plays their sound
- **events and table access**: the table script calls into pages, pages call
  back into the table script, and pages can read and change the table

```
 VPX process                                          Chromium helper processes
 ┌──────────────────────────────────────────────┐     ┌─────────────────────────┐
 │ table script                                 │     │ WebView Helper          │
 │   WebView.Open / Place / Send                │     │   (Renderer): injects   │
 │   Sub WebView_OnEvent                        │     │   window.vpx into pages │
 │        ▲                │                    │◀───▶│   (GPU), (Plugin), ...  │
 │ plugin-webview          ▼                    │ IPC │                         │
 │   CEF browser process, pumped on VPX's main  │     └─────────────────────────┘
 │   thread                                     │
 │   OnPaint        → compositor thread         │
 │                    → screens: displays       │
 │                      + windows               │
 │   OnAudioStream  → audio stream per page     │
 │   vpx.emit       → RunScript(EventHandler)   │
 │   vpx.table      → RunScript, answered       │
 │                    through WebView.Bridge    │
 └──────────────────────────────────────────────┘
```

## What it can be used for

It is a general way to get anything the web can draw or play onto a table:

- flasher, insert and GI effects from shaders, particles or video, driven by
  table events instead of baked image sequences
- LCD and video displays: apron screens, speaker panels, toys with screens,
  full color DMD replacements, backglass and topper animations
- a show layer: a page can be the table's media controller, with scenes,
  music and callouts, while the script only sends game events
- score displays, attract mode, operator and settings menus
- reuse of a real machine's display software, when its media controller is a
  web app (for example over MPF's BCP)

## Build

Same layout as vpinball: `platforms/config.sh` pins the dependencies, and each
platform has its own `platforms/<platform>-<arch>/external.sh`. That script
fetches the VPX plugin SDK headers for `VPINBALL_SHA` into
`third-party/include/plugins`. It also downloads the CEF minimal binary
distribution for `CEF_VERSION` from cef-builds.spotifycdn.com, verifies its
SHA1, and builds `libcef_dll_wrapper` into `external/<platform>-<arch>/<BUILD_TYPE>/cef`.
CMake then builds the plugin and the helper, and stages the plugin folder at
`build/<BUILD_TYPE>/webview`.

```sh
BUILD_TYPE=Release ./platforms/macos-arm64/external.sh
cmake -DPLATFORM=macos -DARCH=arm64 -DCMAKE_BUILD_TYPE=Release -B build/Release
cmake --build build/Release -- -j$(sysctl -n hw.ncpu)
mkdir -p ~/Library/Application\ Support/VPinballX/10.8/plugins
cp -a build/Release/webview ~/Library/Application\ Support/VPinballX/10.8/plugins/
```

| Platform | Script | CMake |
|---|---|---|
| macOS arm64 / x64 | `platforms/macos-arm64`, `platforms/macos-x64` | `-DPLATFORM=macos -DARCH=arm64` / `x64` |
| Linux x64 / aarch64 | `platforms/linux-x64`, `platforms/linux-aarch64` | `-DPLATFORM=linux -DARCH=x64` / `aarch64` |
| Windows x64 / x86 | `platforms/windows-x64`, `platforms/windows-x86` | `-DPLATFORM=windows -DARCH=x64 -G "Visual Studio 18 2026" -A x64` / `-A Win32` |

Windows builds use MSVC because CEF ships MSVC binaries and does not support
MinGW. The plugin talks to VPX only through the C plugin API, so the MSVC DLL
should also load in the MinGW build of VPX. Set `VPINBALL_SOURCE_DIR` to build
against a local vpinball checkout instead of the pinned SDK. The workflow in
`.github/workflows/build.yml` runs the same steps for every platform.

The staged folder is about 330 MB on macOS, almost all of it Chromium:

```
webview/                                    macOS
  plugin.cfg
  plugin-webview.dylib
  WebView.app/Contents/Frameworks/
    Chromium Embedded Framework.framework/
    WebView Helper.app/                     plus (Alerts), (GPU), (Plugin), (Renderer)

webview/                                    Windows, Linux
  plugin.cfg
  plugin-webview.dll | .so
  webview-helper(.exe)
  libcef.dll | libcef.so, *.pak, icudtl.dat, locales/, ...
```

## Settings

```ini
[Plugin.WebView]
Enable = 1
; AllowExternalUrls = 1   ; let pages load from the internet and from files outside the table folder
; DevTools = 1            ; remote debugging on port 9222, open chrome://inspect in Chrome
```

A screen shown on a backglass window competes with other renderers (B2S,
PUP...). Raise its priority with `Priority.WebView` in the `[Backglass]`,
`[ScoreView]` or `[Topper]` section. Page audio has a gain setting in the
mixer (`AudioSource.WebView.Gain`).

## Table script

Two kinds of things:

- **Screens** are what VPX sees. A screen is a virtual canvas with a size and
  a number. Flashers in *Display* mode show screen `N` through the image source
  link `ctrl://webview/display?id=N`, and a screen can also be shown on the
  backglass, score view or topper window.
- **Pages** are web content, each with a name. A page has no output of its
  own. Place it on one or more screens at a position and layer. A page that
  is never placed still runs its scripts, table access, events and audio, so
  it can be a pure logic or sound page.

```vbscript
Dim WebView
Set WebView = CreateObject("WebView.Controller")
WebView.EventHandler = "WebView_OnEvent"

WebView.Screen 1, 512, 256                          ' flasher screen: ctrl://webview/display?id=1
WebView.Screen 2, 1280, 1024
WebView.ShowScreen 2, "backglass"                   ' "backglass", "scoreview", "topper", comma separated, or ""

WebView.Open "fx", "fx/index.html"                  ' page name, url relative to the table folder
WebView.Place "fx", 1, 0, 0, 512, 256               ' page, screen, x, y, width, height
WebView.Place "fx", 2, 0, 0, 1280, 1024             ' same page on a second screen

WebView.Open "overlay", "overlay/index.html"        ' transparent page drawn over the first one
WebView.Place "overlay", 2, 0, 820, 1280, 200
WebView.SetLayer "overlay", 1

WebView.Open "logic", "logic/index.html"            ' never placed: runs without a picture

WebView.Send "overlay", "score", "125000"           ' delivered to vpx.on('score', cb) in the page

Sub WebView_OnEvent(page, name, data)               ' from vpx.emit(name, data); also "loaded"
	Debug.Print page & " " & name & " " & data
End Sub
```

| Member | |
|---|---|
| `Screen(id, width, height)` | create or resize screen `id` |
| `CloseScreen(id)` | |
| `ShowScreen(id, windows)` | show the screen on windows; a window shows one screen, so assigning it moves it from the screen that had it |
| `Open(page, url)` | create the page, or navigate it if it exists |
| `Close(page)` | |
| `Place(page, screen, x, y, width, height)` | place or move a page on a screen in screen pixels; width or height `0` removes it from that screen |
| `SetLayer(page, layer)` | drawing order on screens, higher on top (default 0) |
| `Eval(page, js)` | run JavaScript in the page; each call is a separate global script, so wrap snippets in `{ }` to keep `const`/`let` local |
| `Send(page, name, data)` | message to `vpx.on(name)` handlers |
| `SetVolume(page, volume)` | page audio volume, 0 to 4 |
| `SetFrameRate(page, fps)` | default 60 |
| `EventHandler` | name of a `Sub (page, name, data)` that receives page events |
| `Running` | CEF is running |

Placement and compositing:

- A page renders at the size of its largest placement and is scaled to the
  others. Placements with a different shape stretch it, so give screens of a
  different shape their own page.
- Pages should size themselves from `innerWidth`/`innerHeight`, which follow
  the placement.
- Transparent page backgrounds show the layers below. On flashers, which
  have no alpha, black reads as off.
- Screens are composited on the CPU, on their own thread, and only when one
  of their pages repaints. With an animated full page plus an overlay, a
  1280x1024 screen cost about 2 ms per repaint and a 512x256 flasher screen
  about 0.1 ms on an M-series Mac.

## Urls and security

- Relative urls are served from `vpx://table/`, which maps to the folder that
  holds the table and nothing else. A url cannot climb out of it. Symbolic
  links placed in the folder are followed.
- Everything else is refused unless `AllowExternalUrls = 1`: `https://`
  pages, absolute file paths, and also requests and navigations made by
  pages (CDN scripts, `fetch`, links).
- Only pages served from the table folder get `vpx.table`.
- Chromium's sandbox is off on every platform. On macOS that is what lets the
  plugin run from the user plugin folder, outside the VPX application bundle.

## Page side

`window.vpx` is injected into every page before its own scripts run:

```js
vpx.name                                   // the page's name
vpx.on('score', (data, name) => { ... })   // returns an unsubscribe function
vpx.on('*', (data, name) => { ... })
vpx.emit('counted', { value: 125000 })     // objects are JSON encoded
```

Everything a page writes to the console goes to VPX's log, prefixed with the
page name. Pages don't have to know about `vpx`: a table can drive any page
purely through `Eval`, e.g. by calling the page's own functions.

### Table access

`vpx.table` mirrors the table script's names, so page code can run game logic,
not only animations:

```js
const angle = await vpx.table.LeftFlipper.CurrentAngle   // read a part property
vpx.table.gi1.State = 1                                   // write
vpx.table.LeftFlipper.RotateToEnd()                       // call a part method
const total = await vpx.table.AddScore(1000)              // call a table Sub or Function
const balls = await vpx.table.BallsRemaining              // read a table script global
const stop = vpx.watch(['LeftFlipper.CurrentAngle', 'gi1.State'], ([angle, state]) => { ... })
```

- Reads are awaited (or use `.then` / `.catch`), because the page runs in
  another process. In testing, a round trip took about 1 ms. Writes are sent
  without waiting, and calls return a promise.
- `vpx.watch` reads its list as one batch per animation frame and only calls
  back when a value changed.
- Values come back as JSON. Numbers, booleans and strings map directly,
  arrays become JS arrays, `Empty`/`Null` and objects become `null`.
- Errors reject the promise (`Object required (424)`). They don't stop the
  table script.
- Pages never send VBScript. They send names and typed values. The plugin
  only accepts plain identifiers as names, converts the values to VBScript
  literals itself, and builds the statement.
- Each request compiles a little VBScript, so this suits a handful of calls
  or watched values per frame, not hundreds.
- Table events still need one line in the table, e.g.
  `WebView.Send "logic", "hit", "Target1"` in `Target1_Hit`.

How it works: on the first request, the plugin installs a small helper into
the table script (a `WebView__Json` encoder and a `WebView.Bridge` object).
Each request then runs as one `RunScript` wrapped in `On Error Resume Next`
and answers through the bridge. Answers carry a token that changes every
game, so nothing else can forge them.

### Audio

Page audio needs nothing special, but the first sound has a start-up cost.
Chromium sets up its audio output the first time a page creates an
`AudioContext`. On macOS that took about 330 ms, during which the page's
animation stalls and the first sound is late. An `AudioContext` that keeps
running, even silent, also cost VPX regular long frames in testing. Create it
when the page loads, suspend it, and resume it when you play:

```js
const audio = new AudioContext()
audio.suspend()

function play(buffer) {
  audio.resume()
  const source = audio.createBufferSource()
  source.buffer = buffer
  source.connect(audio.destination)
  source.start()
}
```

Measured with a sound on the first flipper press:

| | First press → sound | Page stall | Long VPX frames |
|---|---|---|---|
| created on first press | 411 ms | 300 ms | normal |
| created at load, left running | 63 ms | none | 9–17/s |
| created at load, suspended | 83 ms | none | normal |

## Examples

`examples/` is a small, self-contained demo. It needs no assets: sounds are
synthesized, and the effects are drawn on a canvas.

| Page | What it shows |
|---|---|
| `fx/` | flasher effect: a breathing glow, ring bursts and sparks on `burst`, colour changes on `color` |
| `overlay/` | transparent score overlay layered over `fx` on both screens; `score` sets the number |
| `sound/` | sound page with no picture: whoosh, blip and chime on `play`, using a suspended `AudioContext` |
| `logic/` | logic page with no picture: watches `LeftFlipper.CurrentAngle` through `vpx.watch`, switches `gi2` off while the flipper is up, and reports each raise with `vpx.emit` |

`webview-demo.vbs` wires them together:
- screen 1 (512x256) goes to a flasher, screen 2 (1280x1024) to the backglass
- the left flipper fires a burst, plays a whoosh and adds 1000 to the score
- the right flipper changes the colour and plays a blip
- every tenth flipper raise reported by the logic page plays a chime with a bigger burst

The quickest way to run it is the demo table builder, which converts VPX's
blank table, adds the flasher and the script, and prints the command to play
it:

```sh
tools/demo_table.py /path/to/VPinballX_BGFX.app /tmp/webview-demo
```

To add it to your own table instead:
1. Copy the four folders next to the table.
2. Add a flasher in *Display* mode with image source link `ctrl://webview/display?id=1`.
3. Append `webview-demo.vbs` to the table script.
4. Call `WebViewDemo_KeyDown keycode` from `Table1_KeyDown`.

## Testing

`tools/smoke.py /path/to/VPinballX_BGFX.app` builds the demo table, plays it,
presses the flippers and checks the result: pages load, the logic page reads
the table and reports back, the flasher screen has a picture, external urls
and path traversal are refused, injected names are rejected, the log has no
plugin errors, and VPX exits cleanly with no helper process left behind.

It drives VPX through VPX's MCP server, so it only works with a VPX build
that has one, and the plugin must already be installed in that VPX.

## How it runs inside VPX

| File | |
|---|---|
| `src/WebViewPlugin.cpp` | plugin load/unload, settings, end of game |
| `src/CefHost.cpp` | CEF start-up, the main-thread message pump, the `vpx://table/` scheme, url policy |
| `src/Pages.cpp` | pages, screens, the compositor thread, the browser client, displays and windows |
| `src/Audio.cpp` | the mixer source and the audio stream of each page |
| `src/TableBridge.cpp` | page events, `vpx.table` requests |
| `src/Controller.cpp` | the `WebView.Controller` and `WebView.Bridge` script objects |
| `src/helper/HelperMain.cpp` | the Chromium helper process, which injects `window.vpx` |

- **Message loop.** CEF runs with `external_message_pump`. The plugin drives
  `CefDoMessageLoopWork` from VPX's main thread through `RunOnMainThread`, both
  when CEF asks (`OnScheduleMessagePumpWork`) and on a 60 Hz heartbeat. The
  pump is always queued with a tiny delay, because a zero delay from the main
  thread runs immediately and would re-enter CEF.
- **Lifetime.** CEF can be initialized once per process, but VPX loads and
  unloads plugins for every game. The plugin pins its own module on first use,
  starts CEF on the first `Open`, closes all browsers at game end and pauses
  the pump, then resumes it in the next game.
- **macOS.** The framework and the helpers sit in a `WebView.app` bundle
  inside the plugin folder, which CEF is told to use as its main bundle, so the
  plugin works both inside the VPX application bundle and in the user plugin
  folder (`~/Library/Application Support/VPinballX/<version>/plugins`). The
  framework is loaded with `cef_load_library`. The `CefAppProtocol` methods are
  added to VPX's (SDL's) `NSApplication` class at runtime. The helper bundles
  are `LSUIElement`. Local builds are ad-hoc signed, the release builds are
  signed with a Developer ID and notarized.
- **Audio.** Each page is muted with `SetAudioMuted(true)`. CEF keeps playing a
  page's audio to the system output even when an audio handler captures it, so
  without the mute every sound plays twice. `CefAudioHandler` still delivers
  the page's audio while it is muted, as planar float PCM at VPX's output rate
  in 10 ms packets. The plugin interleaves it and hands it to VPX with
  `CTLPI_AUDIO_ON_UPDATE_MSG`. Silence inside a sound is passed on, so its
  timing is kept, and a sound ends after 250 ms of silence. VPX's plugin audio
  API has a single target today (`CTLPI_AUDIO_TARGET_BACKGLASS`), so page
  audio plays on the backglass device.
- **Pixels.** Pages render at `device_scale_factor = 1`. `OnPaint` delivers
  premultiplied BGRA on the main thread, where the plugin only copies the
  frame. The compositor thread blends frames into each screen, sampling
  bilinearly when a frame is scaled, and publishes the screen triple-buffered
  as RGB (displays) and straight-alpha RGBA (windows).

## Known limits

- **Main-thread cost on macOS.** Chromium's loop has to run on VPX's main
  thread. Start-up is rough for the first seconds after the pages open, and in
  steady state a call into Chromium's loop sometimes takes 10 ms, which costs
  a long frame. A profile shows the main thread polling the clock inside CEF's
  own pump at those moments. No fix is known from the plugin side.
- **Frames are copied on the CPU.** With `CefWindowInfo::shared_texture_enabled`
  CEF can deliver frames as a GPU handle (IOSurface, D3D11 shared handle,
  dmabuf), but using it needs a VPX plugin API for native textures, which
  does not exist.
- **Audio goes to the backglass device** only.
- **No input.** Pages get no mouse or keyboard.

## Status

macOS arm64 is built and tested in VPX, including two games in one VPX
process. macOS x64 cross-builds. Windows and Linux have their scripts and
CMake paths, but have never been built or run, and the CI workflow has never
run.
