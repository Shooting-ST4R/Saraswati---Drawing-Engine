# Saraswati — Version 1.0.0 (beta)

A native painting app (Windows first, Linux second) built around a Vulkan GPU brush engine
designed for **no perceptible pen lag**, even on 30,000 × 30,000 px canvases with 5,000 px
brushes. Architecture: [DESIGN.md](DESIGN.md).

## What's in 1.0.0

- **Brushes:** Hard Round, Textured Pen (paper grain fixed to the canvas), Soft Round (airbrush
  build-up). Size 1–5000 px, opacity, flow, spacing, hardness, texture, pressure → size / opacity,
  pressure curve, eraser (also the pen's eraser end). No smoothing is added to your strokes.
- **Pen tablets:** pressure through Windows Ink (Wacom / Cintiq / XP-Pen / Huion …) and
  XInput2 / Wayland on Linux. In the *Wacom Tablet Properties* keep **"Use Windows Ink"** switched
  on (it is on by default), otherwise Windows only reports a mouse without pressure.
- **Tools:** brush, eyedropper (also Alt+click), fill (bucket), gradient, line, rectangle,
  ellipse (outline or filled), rectangle / ellipse / lasso / magic-wand selection, move /
  transform (scale, rotate, 4-corner distort, flip), hand.
- **Layers:** thumbnails, new, duplicate, delete, drag-to-reorder, visibility, opacity, rename,
  lock transparency, and all 28 Clip Studio
  Paint **blend modes** (Normal … Brightness). All layers live in GPU memory; the maximum layer
  count is computed from your VRAM and the document size and shown in *New Document* and *Layers*.
- **Undo / redo** (50 steps, kept in system RAM) for strokes, fills, transforms and layer
  operations (add, delete, duplicate, reorder, visibility, opacity, blend mode, rename, lock).
  Selection changes are undoable too; undoing back to the last save clears the `*`.
- **Safety:** unsaved changes are marked with `*` in the title; quitting, *New* and *Open* ask to
  save first. Saves are atomic (written to a temporary file, then swapped in).
- **Files:** open and save **PSD** (and **PSB** for documents over 30,000 px or 4 GB) with layers,
  names, opacity, visibility and blend modes — the exchange format with Photoshop, Clip Studio
  Paint, Krita etc. Open / import **PNG, JPEG, GIF (first frame), WebP, SVG, BMP, TGA**; drag files
  onto the window to import them as layers (Ctrl+drop opens as a new document).
- **Interface:** dockable panels — drag a panel's tab to an edge to snap it, onto another panel to
  combine them as tabs, drag the separators to resize; *Window › Reset layout* restores the default.
  Neutral-grey theme so the UI does not tint your colour perception. Tab hides all panels.
  The layout, brush settings, colours and last folder are remembered in `User-Data/settings/`.
- **Colour panel:** HSV wheel, foreground/background swatches (X swaps), hex entry, H/S/V in
  degrees/percent, recently used colours. Status bar with tool, cursor position, zoom and rotation.
- **Performance panel** (FPS, GPU time per stage, input → present latency, VRAM, undo RAM,
  present mode) and a built-in **benchmark**.

Not yet (see `../Possible Feature List.txt`): native `.clip` files, layer folders / masks, text,
mesh warp / liquify, selection undo, multi-GPU.

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
| Brush tips | 1 Hard Round · 2 Textured Pen · 3 Soft Round |
| Brush size | [ and ], or Ctrl+Alt+drag on the canvas |
| Swap colour / background colour | X |
| Straight line from the last stroke | Shift+click |
| Eyedropper | I, or Alt+click while painting |
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
| Export flattened PNG / JPEG | Ctrl+E |
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
`--import <file>` (repeatable), `--save <file.psd>`, `--demo`, `--tooltest`,
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
