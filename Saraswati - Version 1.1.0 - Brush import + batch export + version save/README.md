# Saraswati — Version 1.1.0 (beta)

A native painting app (Windows first, Linux second) built around a Vulkan GPU brush engine
designed for **no perceptible pen lag**, even on 30,000 × 30,000 px canvases with 5,000 px
brushes. Architecture: [DESIGN.md](DESIGN.md). What changed in each version:
`../Version Feature Log.txt`.

## What's in 1.1.0

- **Brushes:** a library of 22 brushes in groups (pencils, G-Pen, mapping pen, markers, flat /
  dry brushes, chalk, charcoal, airbrushes, sprays, erasers) in the *Tool Group* panel; size
  1–5000 px, opacity, flow, spacing, hardness, roundness, angle, paper texture, scatter, jitter,
  spray particles, pressure → size / opacity with a pressure curve. Save your own brushes.
  No smoothing is added to your strokes; zoomed-out strokes follow a smooth curve through the
  pen samples. Keys 1–9 pick brushes.
- **Brush import:** Photoshop **.abr** and Clip Studio Paint **.sut** brushes, also **.zip**
  packs (*File › Import brushes*, or drag them onto the window). Tip images, textures, dual tips
  and dynamics are converted; a report lists anything that is only approximated.
- **Pen tablets:** pressure through Windows Ink (Wacom / Cintiq / XP-Pen / Huion …) and
  XInput2 / Wayland on Linux. In the *Wacom Tablet Properties* keep **"Use Windows Ink"** on.
- **Tools:** brush, eraser, eyedropper, fill, gradient, line, rectangle, ellipse, rectangle /
  ellipse / lasso / magic-wand selection with a CSP-style selection bar, move / transform, hand,
  **text**. Holding a tool's key uses it only while held (spring-loaded); Space always pans.
- **Text:** every installed font; size, leading, kerning, tracking, horizontal / vertical scale,
  baseline shift, skew, all caps, underline, strike-through, alignment, indents, paragraph
  spacing. Text stays editable (also after saving and reopening).
- **Speech bubbles (manga):** switch on *Bubble* in the text tool — a bubble is built around the
  text live (shape, distance, fill, border colour / width, opacity). The small tail button next to
  the bubble draws a tail, straight or freehand. The bubble is a sublayer of its text layer.
- **Layers:** folders with their own blend mode and opacity (or pass-through), clipping
  ("clip to layer below") for layers and folders, all 28 CSP blend modes, a **Paper** layer (one
  colour, always at the bottom), lock transparency, thumbnails. *Layer Properties*: the manga
  **tone** effect (30 dot shapes: round, oval, square, diamond, line, cross, noise …; frequency,
  angle, density) and the non-destructive **layer colour** effect.
- **Files:** open / save **PSD** (and **PSB** for huge documents) with layers, folders, clipping,
  blend modes and Saraswati's own extras (tone, layer colour, editable text and bubbles). Every
  save is read back and compared before it replaces the old file. Open / import PNG, JPEG, GIF,
  WebP, SVG, BMP, TGA; drag images onto the canvas or the Layers panel to add them as a layer
  (also straight from a web browser).
- **Version Save and Batch Export** (buttons in the top bar):
  - New pictures are named `2026 09 22 - name - Version 1.psd`. The date is when the picture was
    started and never changes. **Version Save** (Ctrl+Alt+S) writes `Version 2`, `3` … next to
    it.
  - **Batch Export** (Ctrl+Alt+E) writes a list of images next to the .psd in one go, by default
    `… - S100% Q100.jpg`, `… - S100%.png`, `… - S50% Q95.jpg` and `… - S50%.png`. S is the size,
    Q the JPEG / WebP quality; the extension is the format.
  - Right-click the button (or *File › Batch Export settings*) to choose which files are made —
    JPEG, PNG or WebP (lossy or lossless), any size and quality. The list is remembered.
- **Automatic backups** every 5 minutes (adjustable) in the background, without any dialog or
  pause; after a crash the next start offers to restore. *File › Restore from backup* lists all
  versions.
- **View:** flip the view horizontally / vertically (only the view — the picture is not changed),
  rotate, zoom, Navigator panel. FPS limit (120 by default) in *Preferences › Performance*.
- **Interface:** dockable panels, neutral-grey theme, rebindable shortcuts (*Preferences ›
  Shortcuts*), preferences for pen buttons, backups, interface size, FPS limit.

Not yet (see `../Possible Feature List.txt`): native `.clip` files, layer masks, mesh warp /
liquify, multi-GPU.

## Build

### Windows (the main target)

Needs **Visual Studio 2022** (or Build Tools) with *Desktop development with C++* — this includes
the *C++ CMake tools for Windows* component (CMake + Ninja) — and **Git for Windows**.

Double-click **`Build Saraswati.bat`**. It finds Visual Studio (vswhere), downloads the libraries
into `App-Data/Source/third_party/` (first run only, pinned versions), builds a Release
`Saraswati.exe` (static runtime, one file, no DLLs) and copies it to `App-Data/App/`.
The first build takes a few minutes (it also builds SDL3, glslang and libwebp).

### Linux

`./build-linux.sh` (the packages it needs are listed at the top of the script). Result:
`App-Data/App/saraswati`.

## Run

- **`Run Saraswati.bat`** — normal start on your GPU.
- **`Run Saraswati (CPU Vulkan).bat`** — runs on SwiftShader, the CPU Vulkan driver that ships
  with Google Chrome / Microsoft Edge (found automatically). For PCs without a Vulkan GPU, e.g.
  the Windows VM. Correct but slow, and documents are limited to 8192 px per side.
- Linux: `App-Data/App/saraswati` (or `--cpu-vulkan` for Mesa lavapipe).

Your drawings go in `User-Data/documents/` by default; benchmark reports in `User-Data/logs/`.

## Controls

| Action | Keys |
|---|---|
| Brush / eraser toggle | B / E (the pen's eraser end erases automatically) |
| Brushes | 1–9 pick the first nine brushes of the library |
| Brush size | [ and ], or Ctrl+Alt+drag on the canvas |
| Swap colour / background colour | X |
| Straight line from the last stroke | Shift+click |
| Eyedropper | I, or Alt+click while painting, or the pen's upper barrel button |
| Pan with the pen | hold the lower barrel button |
| Fill / gradient | G / Shift+G |
| Line → rectangle → ellipse | U (press again to cycle) |
| Rectangle / ellipse select | M (press again to switch) |
| Lasso / magic wand | L / W |
| Selection: add / subtract | hold Shift / Alt when starting |
| Fill / magic wand across all layers | *Refer to all visible layers* in Tool Settings (fill flat colours under line art) |
| Select all / deselect / invert | Ctrl+A / Ctrl+D (or Esc) / Ctrl+Shift+I |
| Clear / fill selection (or layer) | Delete / Alt+Backspace |
| Move / transform | V or Ctrl+T · corners scale (Shift keeps ratio) · Ctrl+corner distorts · outside rotates · Enter applies, Esc cancels |
| Pan | Space+drag, middle mouse, or H |
| Rotate view | Shift+Space+drag · R resets |
| Zoom | mouse wheel (at the cursor) · + / − · Ctrl+0 fit · Ctrl+1 100 % |
| Hide / show all panels | Tab |
| Undo / redo | Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) |
| New / open / import / save / save as | Ctrl+N / Ctrl+O / Ctrl+I / Ctrl+S / Ctrl+Shift+S |
| Export flattened PNG / JPEG / WebP | Ctrl+E |
| Version Save / Batch Export | Ctrl+Alt+S / Ctrl+Alt+E |
| Text tool | T · click to type, drag to make a text frame · Esc finishes |
| Temporary tool | hold its key (e.g. E), use it, release — back to the previous tool |
| Layers | drag a row to reorder · double-click to rename · eye icon toggles visibility |

## Performance test (RTX 3090 PC)

The benchmark plays a synthetic 4-second zig-zag pen stroke (240 Hz, varying pressure) across the
whole canvas in real time, then writes a report to `User-Data/logs/benchmark-<time>.txt` (and the
console, when started from a terminal). From a command prompt in the version folder:

```
App-Data\App\Saraswati.exe --benchmark --doc 30000x30000 --brush 5000 --tip hard --exit
App-Data\App\Saraswati.exe --benchmark --doc 30000x30000 --brush 5000 --tip soft --layers 4 --exit
App-Data\App\Saraswati.exe --benchmark --doc 12000x8000 --brush 300 --tip textured --exit
```

Or use *Performance › Run benchmark* inside the app. What to look at in the report: *frame ms*
(p95 should stay under your monitor's frame time, 6.9 ms at 144 Hz), *GPU ms* and
*input → present* latency. Numbers under CPU emulation are meaningless for performance.

A 30,000 × 30,000 layer is 3.6 GB of VRAM (plus 1.8 GB stroke mask and 0.9 GB selection), so
a 24 GB card holds about 4–5 layers at that size — the app tells you the exact limit.

## Command line

`--cpu-vulkan`, `--vulkan-driver <dll/so>`, `--gpu <index>`, `--validation`, `--doc WxH`,
`--brush PX`, `--tip hard|textured|soft`, `--layers N`, `--benchmark`, `--open <file>`,
`--import <file>` (repeatable), `--save <file.psd>`, `--demo`, `--tooltest`, `--brushtest`,
`--brushes <file.abr|.sut|.zip>`, `--drop <file>` (same as dropping it on the canvas), `--batchtest`,
`--screenshot <file.png>`, `--frames N`, `--window WxH`, `--exit`. `--help` lists them.

## Notes for the Windows build / test step

- Built and tested in a Linux cloud session (GCC 13, Mesa lavapipe under Xvfb, Vulkan validation
  layers clean). The **Windows build was cross-compiled with MinGW-w64** (static, all `_WIN32`
  code paths) and **ran correctly under Wine** (the full `--tooltest` scene matched Linux
  pixel-for-pixel). The MSVC build itself has not been run yet — if `Build Saraswati.bat` fails,
  expect a small compiler-specific fix. (Cross builds pass `-DSARASWATI_GLSLANG=<host glslang>`.)
- Pen pressure could not be tested without a tablet: please try a few strokes with the Cintiq and
  check the *Tool Settings › Pen pressure* curve preview — its dot follows the live pen pressure.
- SwiftShader check on the VM: `"Run Saraswati (CPU Vulkan).bat"`, or for an automated check:
  `Saraswati.exe --cpu-vulkan --demo --screenshot demo.png --exit` and `--tooltest` likewise.
  The *Performance* panel should show "SwiftShader" and "CPU emulation".
- The test helpers `--demo` / `--tooltest` draw a fixed scene (all brushes, layers, blend modes,
  undo/redo, every tool) — compare the screenshot with what the README describes.
- Saving: the GPU readback happens on the main thread (a short pause on big documents); PSD
  encoding and disk writes run on a worker thread. Fill / magic wand / transform read the layer
  back only the painted area and flood on a worker thread (the UI stays responsive; the status bar
  shows *Working…*).

## Known issues

- *Fill / magic wand with "Refer to all visible layers"* on huge documents flattens the whole
  painted area in one GPU submission; on a 30k × 30k document this may briefly hitch the display
  (the UI thread itself never waits). Planned: split it into a few tiles per frame.
- The vertical size / opacity sliders on the tool strip are small at 720p (use the Tool Settings
  sliders or `[` `]` for precise values).
- Barrel buttons: Wacom's driver must leave the buttons on their *default* pen functions (not
  "right click") for pan / eyedropper to work.
- A fill or wand result is dropped (with a message) if anything changes while it is computed —
  just click again.
- Not yet verified on real hardware: MSVC build, Windows Ink pressure feel on the Cintiq,
  performance on the RTX 3090 (run the benchmark above).
