# Saraswati 1.0.0 — design and build plan

First prototype of a lag-free painting engine. Scope is deliberately small: prove
the engine architecture, not feature parity with Clip Studio Paint.

## Scope of 1.0.0

- One document: width × height chosen in a New Document dialog, paper colour
  white or transparent.
- Three brushes, each with basic settings:
  1. **Hard Round** — anti-aliased hard disc (a clean inking pen).
  2. **Textured Pen** — hard round pen (CSP G-pen feel: pressure → size) whose
     coverage is modulated by a paper-grain texture fixed in canvas space.
  3. **Soft Round** — airbrush-like soft falloff with hardness, builds up with
     flow inside a stroke.
- Settings: size (1–5000 px), opacity, flow, spacing, hardness (soft),
  texture strength + scale (textured), pressure → size (with min size %),
  pressure → opacity, pressure curve (gamma), colour (HSV picker), eraser
  toggle (also automatic with the pen's eraser end).
- Layers: new, delete, duplicate, move up/down, select, visibility, opacity,
  rename. Normal blend mode only. **All layers live in VRAM**; the maximum layer
  count is computed from VRAM and document size and shown in the New Document
  dialog and the layer panel (Procreate-style).
- Undo / redo for strokes (tile-based, stored in system RAM).
- View: zoom at cursor, pan, rotate, fit to window, 100 %.
- Performance panel + benchmark mode (see below).
- Added during the 1.0.0 build at the user's request (see "Additions" below):
  28 CSP blend modes, PSD/PSB open + save, image import, docking UI with a
  neutral-grey theme, and the basic tool set (selections, fill, gradient,
  line/shapes, eyedropper, transform).
- Not in 1.0.0: native .clip files, layer folders/masks, text, mesh warp,
  mipmaps, tiled/sparse layers, multi-GPU. (Next versions; see
  `Possible Feature List.txt` at the project root.)

## Stack

C++20 · CMake (≥ 3.24) · Vulkan 1.1+ via **volk** (runtime-loaded, so the app
can load a specific driver DLL) · **SDL3** (window, Vulkan surface, pen events)
· **Dear ImGui** (SDL3 + Vulkan backends, `IMGUI_IMPL_VULKAN_USE_VOLK`) ·
**glslang** built from source as a build tool; shaders compiled with
`glslang -V --vn <name>` into C headers and embedded. Static CRT on MSVC so the
.exe is a single file. Everything must build on Windows (MSVC) and Linux (GCC/Clang).

Pinned dependency commits (fetched by `App-Data/Source/fetch-deps.sh` / `.ps1`):

| Dir | Repo | Commit |
|---|---|---|
| SDL | libsdl-org/SDL | a19bac65218bf583e8e5cb8c52e1c9603fa8d94c |
| imgui | ocornut/imgui | 3bae66c735670619baf51391eba7f3d90a25d125 |
| Vulkan-Headers | KhronosGroup/Vulkan-Headers | 3c65a01745e4a1134d32b9c2c456472212dba16d |
| volk | zeux/volk | 7f46f79751d7e3b3a6df20e38d3e3986585bcdf4 |
| glslang | KhronosGroup/glslang | 2ff6f609379ce43c4291c732cf6a19dd2461a680 |
| stb | nothings/stb | 2c980bb59875b0d32144a71867fbdebb2f77cd20 |
| nanosvg | memononen/nanosvg | 239e102ec2c691f2902e20ace2ed36ee4a35cfe6 |
| libwebp | webmproject/libwebp | 4fa21912338357f89e4fd51cf2368325b59e9bd9 (v1.6.0) |

glslang: `ENABLE_OPT=OFF`, tests off. SDL: static only.

## Driver selection

- Default: system Vulkan loader (`vulkan-1.dll` / `libvulkan.so.1`), prefer a
  discrete GPU; `--gpu <index>` overrides.
- `--vulkan-driver <path>`: `SDL_Vulkan_LoadLibrary(path)` on a driver/loader
  DLL, then `volkInitializeCustom(SDL_Vulkan_GetVkGetInstanceProcAddr())`.
- `--cpu-vulkan`: Windows — find `vk_swiftshader.dll` under
  `Program Files/Google/Chrome/Application/<ver>/` or
  `Program Files (x86)/Microsoft/Edge/Application/<ver>/`, load it as above.
  Linux — set `VK_DRIVER_FILES`/`VK_ICD_FILENAMES` to lavapipe's ICD json
  (search `/usr/share/vulkan/icd.d/lvp_icd*.json`) before the loader starts.
- Show the device name, driver and "CPU emulation" state in the UI.

## GPU resources

- **Layer**: `VkImage` `R8G8B8A8_UNORM`, premultiplied alpha, usage
  STORAGE | SAMPLED | TRANSFER_SRC | TRANSFER_DST, device-local, own dedicated
  allocation, kept in `VK_IMAGE_LAYOUT_GENERAL` for its whole life.
- **Stroke mask**: one document-size single-channel image. Prefer `R16_UNORM`
  with STORAGE_IMAGE support, fall back to `R32_SFLOAT` (compile shader
  variants with `-DMASK_R32F`). Holds the coverage of the stroke in progress.
- **Screen caches**: two window-size `R8G8B8A8_UNORM` images — `below` (paper +
  all visible layers under the active one) and `above` (all visible layers over
  it). Rebuilt only when the view or the layer stack changes, never during a
  stroke. Recreated with the swapchain.
- **Dab ring buffer**: host-visible SSBO per frame in flight
  (`struct Dab { vec2 pos; float radius; float alpha; }`).
- Descriptor sets: set 0 = global per frame slot (mask storage + sampler, dab
  SSBO, caches as storage + sampler); set 1 = per layer (storage + sampler).
  One pipeline layout for everything, 128-byte push constants.

## Frame flow (single queue, 2 frames in flight)

1. Poll all SDL events. Pen/mouse samples → brush engine → pending dabs.
2. Wait for this slot's fence, acquire the swapchain image.
3. Record:
   - **Dabs** (compute): group consecutive dabs while union-bbox area
     ≤ 2 × summed dab areas and ≤ 64 dabs; one dispatch (16×16) per group over
     its bbox; each pixel loops over the group's dabs. Barrier between groups.
   - **Stroke end**: copy dirty tiles of the active layer to an undo buffer
     (see Undo), barrier, then **commit** (compute over the stroke bbox:
     `layer = colour·a + layer·(1−a)` with `a = mask·opacity`, or
     `layer ·= 1−a` for the eraser) and clear the mask in the same pass.
   - **Cache rebuild** if dirty: init pass (paper colour inside the document,
     transparent elsewhere), then one dispatch per visible layer blending it in.
   - **Composite** (render pass on the swapchain, full-screen triangle):
     `above over (active over below)` where `active` = the active layer with the
     live stroke applied (`stroke over layer`, times layer opacity), then over a
     checkerboard inside the document / workspace grey outside. ImGui draws in
     the same render pass.
4. Submit, present. Render only when something changed; otherwise
   `SDL_WaitEventTimeout` so the app idles at ~0 % CPU.

Present mode: MAILBOX if available (default), else FIFO; the user can pick
FIFO / MAILBOX / IMMEDIATE in the performance panel.

### Sampling (screen → document)

`doc = pan + R(−θ)·(screenPx − screenCentre) / zoom` (pan = document point at
the window centre). Shaders use `texelFetch` only (no format filter features
needed):
- zoom ≥ 4: nearest; 1 ≤ zoom < 4: manual bilinear;
- zoom < 1: n × n stratified point samples per screen pixel,
  `n = clamp(ceil(1/zoom), 1, 6)` (box filter). Mipmaps come later.

## Brush engine (CPU side)

- Samples: document position, pressure (0–1; mouse = 1), timestamp.
- Walk each segment from the previous sample, emitting a dab every
  `spacing = max(0.5, spacing% × current diameter)`; carry the remainder across
  segments; interpolate pressure linearly.
- `p' = pow(pressure, gamma)`; radius = size/2 × (pressure→size ?
  mix(minSize, 1, p') : 1); dab alpha = flow × (pressure→opacity ? p' : 1).
  Radius below 0.5 px → clamp to 0.5 and scale alpha by area.
- Accumulation into the mask: Hard Round / Textured Pen use `max(mask, a)`
  (even strokes, CSP-like); Soft Round uses build-up `mask + a·(1 − mask)`.
- Tip shapes (GPU): hard = `clamp(r − d + 0.5, 0, 1)`; soft = smoothstep
  falloff from hardness·r to r; textured = hard × `mix(1, grain, strength)`
  with `grain` = 3-octave value-noise fBm (integer hash) at `pos / scale`,
  shaped with smoothstep(0.25, 0.75).
- Tracks the stroke bbox and a set of dirty 256 × 256 tiles.
- No smoothing/stabilisation.

## Undo

- On commit, copy only the stroke's dirty tiles of the layer into host-visible
  buffers (chunked, ≤ 256 MB per allocation) with one
  `vkCmdCopyImageToBuffer` per chunk.
- Undo/redo = swap: copy the current tiles into a new buffer, copy the stored
  tiles back into the image, keep the new buffer as the opposite entry; free
  old buffers once that frame's fence has signalled.
- Budget: ≤ 50 steps and ≤ min(25 % of host memory heap, 16 GB); drop the
  oldest entries. Deleting a layer drops its entries. A new stroke clears redo.

## Layer limit

`layerBytes` = `vkGetImageMemoryRequirements` of a probe image at document
size. `usable = deviceLocalHeap × 0.9 − 512 MB − maskBytes − screen caches`
(use `VK_EXT_memory_budget` if present). `maxLayers = clamp(floor(usable /
layerBytes), 0, 999)`. Refuse documents larger than `maxImageDimension2D`, or
whose layer exceeds `maxMemoryAllocationSize`, or with `maxLayers < 1`. Handle
`VK_ERROR_OUT_OF_DEVICE_MEMORY` gracefully (message, no crash).

## Input

- SDL3 pen events: `SDL_EVENT_PEN_DOWN/UP/MOTION/AXIS` (pressure from
  `SDL_PEN_AXIS_PRESSURE`, eraser via `SDL_PEN_INPUT_ERASER_TIP`). Ignore mouse
  events whose `which == SDL_PEN_MOUSEID` for drawing (they still drive ImGui).
- Window created with `SDL_WINDOW_HIGH_PIXEL_DENSITY`; convert event
  coordinates with the pixel density.
- Don't start a stroke when ImGui wants the mouse.
- Shortcuts: 1/2/3 brushes, E eraser, [ ] size, Ctrl+Z / Ctrl+Y (and
  Ctrl+Shift+Z), Space+drag pan, middle-drag pan, Shift+Space+drag rotate,
  wheel zoom at cursor, Ctrl+0 fit, Ctrl+1 100 %, R reset rotation.
- Brush outline circle at the cursor (ImGui foreground draw list).

## Performance panel + benchmark

- Panel: FPS, CPU frame ms, GPU ms per stage (timestamp queries: dabs, commit,
  caches, composite — if the device supports timestamps), input → present-call
  latency (last / average), dabs per frame, VRAM used by layers vs budget,
  undo RAM, device, present mode selector.
- `--benchmark [--doc WxH] [--brush PX] [--tip hard|textured|soft]
  [--layers N] [--exit]`: plays a synthetic zig-zag stroke across the canvas in
  real time (240 Hz simulated pen, ~4 s), then writes a report (frame ms
  avg/p95/max, GPU ms avg/p95, dabs, dab-megapixels/s, device, doc/brush size)
  to `User-Data/logs/benchmark-<timestamp>.txt` and stdout. Also a
  "Run benchmark" button in the panel.
- `--screenshot <file>` (test helper): after the benchmark or after N frames,
  read back the swapchain image and save a PNG/PPM so automated tests can
  check the result visually.

## Milestones (each ends building, committed and pushed)

1. Deps + CMake + window + Vulkan device + swapchain + ImGui on lavapipe.
2. Document, layers, screen caches, composite, view controls.
3. Brush engine + three tips + stroke mask + commit + pen input.
4. Undo/redo, layer panel features, layer limit, New Document dialog.
5. Performance panel, benchmark mode, screenshot helper, automated test runs
   (xvfb + lavapipe), README, launchers (`Build Saraswati.bat`,
   `Run Saraswati.bat`, `Run Saraswati (CPU Vulkan).bat`), Linux build script.

The Windows .exe is built afterwards on the Windows VM (MSVC) and checked on
SwiftShader; real performance is measured on the RTX 3090 PC.

## Additions made during the 1.0.0 build

- **Blend modes.** `view.glsl: blendLayer()` implements CSP's 28 modes with the
  W3C compositing formula on premultiplied colour (Glow dodge / Add (Glow) use
  the premultiplied source so soft edges "glow"). Because non-Normal layers are
  not associative, the frame composite moved to compute (`frame.comp`) writing a
  window-size `work` image: `blend(below, active)` then the `above` cache when
  every visible layer above is Normal, else each above layer is blended per
  frame (still bounded by screen pixels). `present.frag` draws `work` over the
  checkerboard.
- **Selection.** Document-size R8 (or R32F) image in set 0 binding 5, CPU copy
  is the master. Commit and the live stroke multiply coverage by it; the frame
  pass draws marching ants. Selection edits are not undoable yet.
- **Coverage painting.** Fill, filled shapes, gradient, Edit › Fill / Clear
  write 8-bit coverage (or the gradient shader) into the stroke mask and reuse
  the normal commit — so they get undo and selection clipping for free.
- **Transform.** The selection/layer content is read back, lifted (an eraser
  commit), shown as an ImGui textured quad while editing, then drawn back by
  `stamp.comp` through the inverse homography of the 4 corners (bilinear).
  Cancel = undo-and-discard of the lift.
- **Files.** `fileio.cpp`: PSD/PSB reader/writer (RLE, multi-threaded encode,
  tight layer bounds, PSB above 30000 px or 4 GB) + stb_image / libwebp /
  nanosvg import. Saving reads layers back on the main thread and encodes on a
  worker thread.
- **UI.** Dear ImGui docking branch (pinned commit changed accordingly), layout
  in `User-Data/settings/layout.ini`, neutral grey theme (R = G = B), Roboto
  embedded at build time.
