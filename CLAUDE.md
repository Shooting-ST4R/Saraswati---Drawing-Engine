# CLAUDE.md — Saraswati (Project 31, Clip Studio Paint alternative)

Saraswati is a native desktop painting app (Windows first, Linux second — never
a browser/Electron app) whose whole reason to exist is a brush engine with **no
perceptible pen lag**, even on 30,000 × 30,000 px canvases with 5,000 px brushes.
Reference hardware for performance targets: Ryzen 9 5900X, RTX 3090 (24 GB),
64 GB RAM. The engine design is in each version folder's `DESIGN.md`.

## Versioning Rule (MANDATORY — YOU MUST FOLLOW THIS)

This project uses manual, folder-based versioning instead of editing files in place. Every version of the project lives in its own numbered folder. The folder with the highest version number is always the current, active version.

Folder name format: **`Saraswati - Version X.Y.Z - Short description`**
(e.g. `Saraswati - Version 1.0.1 - Fixed soft brush edge`). **No commas** in folder names
(GCC's `-Wl,` linker options split on commas and the Linux build fails); use `+` or "and". The very first
folder, `Saraswati - Version 1.0.0`, was named by the user without a
description; every later folder gets one.

### Before making ANY edit — no matter how big or small

1. Find the current highest-numbered version folder.
2. Copy that entire folder to a new folder with the next version number.
3. Make the edit inside the **new** folder only.
4. Leave the old version folder completely untouched.

Do this even for a single-character fix, a rename, a comment change, or a config tweak. There is no such thing as an edit too small to version.

**Exception — the version currently being built:** a version folder stays open
for edits until the work session that created it has finished and handed it to
the user (e.g. `Saraswati - Version 1.0.0` stays open until the first prototype
is delivered). After that it is frozen like every other past version.

### Rules

- **Never** edit files inside a finished version folder — treat every past version folder as read-only and permanent.
- **Always** create the new version folder first, then edit — never edit first and copy after.
- Do not skip version numbers and do not reuse a version number.
- Do not delete or overwrite old version folders. They're the project history.
- If unsure which folder is the current highest version, list the project directory and check before doing anything else.
- After creating a new version, all further work in that session (further edits, building, running, testing) happens inside the newest folder.
- Git (below) records history too, but it does **not** replace this rule.
- `… - Release` folders (see the Release Rule) are **not** version folders: they never
  count as the "current highest version", and nothing is ever developed in them.

## Release Rule (MANDATORY — how every version is delivered)

Every finished version gets a ready-to-run release folder next to its version folder,
so the user can start the app directly after a `git pull` — no build step, no zip.

1. When a version is finished (tested and handed to the user), build the Windows app from
   that version's committed source.
2. Create **`Saraswati - Version X.Y.Z - Release`** at the project root (same X.Y.Z as the
   version folder it was built from; no description in the name).
3. Put the **unzipped** portable app in it — never a zip, never the source:
   ```
   Saraswati - Version X.Y.Z - Release/
     App-Data/App/Saraswati.exe       the built app (single static exe)
     User-Data/documents/.gitkeep     empty; the user's drawings go here
     User-Data/logs/.gitkeep          empty; benchmark reports go here
     Run Saraswati.bat
     Run Saraswati (CPU Vulkan).bat
     README.md                        how to start / test, plus build info: version, source
                                      commit, compiler, what was verified and what was not
   ```
4. **Commit it to git including the exe** (the only place binaries are committed) and push.
5. Release folders are read-only once created. A fix never goes into a release folder: make
   a new version folder (X.Y.Z+1), then a new release folder from it. Old releases stay.
6. Before committing, the release is smoke-tested (Windows PC if available; otherwise
   Wine + CPU Vulkan in the cloud), and any test output is removed from its `User-Data/`.

How to build the exe:
- **On Windows:** `Build Saraswati.bat` in the version folder (MSVC), then copy
  `App-Data/App/Saraswati.exe` into the release folder.
- **In a Linux cloud session:** cross-compile with MinGW-w64
  (`x86_64-w64-mingw32-g++-posix`, CMake toolchain file with `CMAKE_SYSTEM_NAME Windows`,
  `-DCMAKE_EXE_LINKER_FLAGS=-static`, and `-DSARASWATI_GLSLANG=<host-built glslang>` so the
  shaders compile on the host), then run `Saraswati.exe --tooltest --screenshot … --exit`
  under Wine with `VK_DRIVER_FILES` pointing at lavapipe to verify it.

## Version Feature Log Rule (MANDATORY)

`Version Feature Log.txt` at the project root lists, for every version, what was added,
changed and fixed, in plain language for the user (newest version at the bottom). **Update it
with every version** — while the version is being built and again before its release folder is
made — so the log always matches what the version really contains. Never rewrite the entries of
past versions except to correct a mistake.

## Folder Layout (inside every version folder)

```
Saraswati - Version X.Y.Z - Description/
  App-Data/
    Source/          the app's source: CMakeLists.txt, src/, shaders/,
                     fetch-deps scripts, third_party/ (downloaded, not in git)
    App/             a built portable app (Saraswati.exe / saraswati), once
                     one has been built; don't assume it's present
  User-Data/
    documents/       the user's drawings (native format, once saving exists)
    logs/            benchmark results and logs written by the app
  DESIGN.md          engine architecture and the plan for this version
  README.md          what this version is, how to build and run it
  Build Saraswati.bat
  Run Saraswati.bat
  Run Saraswati (CPU Vulkan).bat
```

- **All code edits happen inside `App-Data/Source/`.**
- **`App-Data/Source/third_party/`** holds the libraries (SDL3, Dear ImGui, Vulkan-Headers, volk, glslang) as shallow git clones at pinned commits. They are not committed; `fetch-deps` recreates them. When copying a version forward, `third_party/` and `build*/` may be left out (re-fetch/rebuild instead) purely to save time/space; everything else comes along.
- **The app finds `User-Data/`** by walking up from its executable's folder until it finds a sibling `User-Data` folder, so the three-part layout must stay intact.
- `User-Data/documents/` is a version folder's own working copy of the user's drawings, not shared across version folders — carrying it forward when versioning keeps in-progress work available in the new version.

## Tech stack and engine rules

- **C++20, CMake, Vulkan** (loaded at runtime through volk), **SDL3** for the window and pen/tablet input (Windows Ink / WM_POINTER on Windows, XInput2/Wayland tablet on Linux), **Dear ImGui** for panels. Shaders are GLSL compiled to SPIR-V at build time by glslang and embedded in the executable.
- **Latency comes first, throughput second.** Never block the input/present path on full-resolution work, undo copies, file I/O or cache rebuilds. The app must never add stabilisation/smoothing by default.
- Brush dabs, layer compositing and display are GPU work (compute + fragment shaders). CPU cores are for input processing, undo bookkeeping, file I/O, compression.
- Display cost must stay bounded by **screen pixels**, not canvas pixels.
- Layers are dense GPU images for now (all in VRAM); the layer limit is derived from VRAM size and document size (Procreate-style) and shown to the user.

## Testing without a real GPU (CPU Vulkan emulation)

The same Vulkan code runs on a CPU Vulkan driver for development/testing:

- **This VM (Windows, no GPU):** Google's SwiftShader, which ships with Chrome and Edge (`vk_swiftshader.dll`). Launch the app with `--cpu-vulkan` (it finds the DLL itself) or `--vulkan-driver <path to dll>`. SwiftShader limits textures to 8192 px, so big-canvas tests only run on real hardware.
- **Linux / cloud sessions:** Mesa lavapipe (`mesa-vulkan-drivers`) + a virtual X display (`xvfb-run`).
- CPU-emulated numbers are for correctness only. **Performance is judged on the user's RTX 3090 PC**, using the app's built-in `--benchmark` mode.

## Git, GitHub and cloud sessions

- Repo: `https://github.com/Shooting-ST4R/Saraswati---Drawing-Engine` (private). The repo root is this project folder (all version folders + `Testing-Data/` + this file).
- Commit identity (repo-local config): `Shooting-ST4R <153090773+Shooting-ST4R@users.noreply.github.com>`.
- Never commit `third_party/`, `build*/`, binaries in a version folder's `App-Data/App/`, or the contents of `User-Data/documents/` and `User-Data/logs/`. **Exception:** the exe in a `… - Release` folder is committed (see the Release Rule).
- **Cloud sessions spend the user's cloud credits.** Ask the user before starting one, respect the budget cap they give, and work in milestones that each leave the repo in a building, committed state.
- Never log in or handle credentials for the user; GitHub logins on the VM go through Git Credential Manager's own window.

## Testing Data Rule (MANDATORY — YOU MUST FOLLOW THIS)

When creating data for testing purposes (sample documents, fixture files, benchmark configs, mock inputs, etc.), never create it directly inside a version folder's `User-Data/`. Instead:

1. Store all testing projects/fixtures in the project-root-level `/Testing-Data` folder, sitting alongside the version folders. This folder is shared across versions, is **not** itself a version, and is never copied forward as part of the Versioning Rule above.
2. If a testing project actually needs to be run/used against a version of the app: copy the relevant testing project(s) from `/Testing-Data` into that version's `User-Data` (e.g. `User-Data/documents/test/`), keeping it clearly separated from any real user data.
3. Use it there to run the test.
4. Once done, **delete the copy from `User-Data` again** — the working copy inside `User-Data` is always temporary and must be cleaned up after use. Never leave test data sitting in `User-Data`.
5. The master copies in `/Testing-Data` are never modified or deleted as part of this — only the temporary copy inside `User-Data` gets deleted.
6. `/Testing-Data` must contain a `README.md` documenting the scope and purpose of each testing project/dataset stored there. Update this file whenever a new testing project is added or an existing one's purpose changes.
