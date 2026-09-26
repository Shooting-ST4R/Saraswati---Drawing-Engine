# Saraswati 1.0.0 (beta) — Windows release

Ready-to-run build of **Saraswati - Version 1.0.0**. No installation, no zip: start it straight
from this folder. Source, design notes and the full manual are in the version folder
`../Saraswati - Version 1.0.0/` (see its README.md for all controls).

## Start

Double-click **`Run Saraswati.bat`** (or `App-Data\App\Saraswati.exe` — Windows may show it as
just "Saraswati", type *Application*).

- "Windows protected your PC"? The exe is not code-signed: click **More info → Run anyway**.
- Needs a current GPU driver with Vulkan (every recent NVIDIA / AMD / Intel driver has it).
- No Vulkan GPU (e.g. a VM)? Use **`Run Saraswati (CPU Vulkan).bat`** — slow, correctness only.
- Keep this folder's layout: the app stores your drawings in `User-Data\documents\`, benchmark
  reports in `User-Data\logs\` and your layout/brush settings in `User-Data\settings\`.

## Pen tablet (Wacom / Cintiq)

In *Wacom Tablet Properties* keep **"Use Windows Ink"** on and leave the pen buttons on their
default functions (lower button = pan, upper button = pick colour). In *Tool Settings › Pen
pressure* the dot on the curve follows your live pen pressure.

## Performance test

From a Command Prompt in this folder:

```
App-Data\App\Saraswati.exe --benchmark --doc 30000x30000 --brush 5000 --tip hard --exit
App-Data\App\Saraswati.exe --benchmark --doc 30000x30000 --brush 5000 --tip soft --layers 4 --exit
```

The report is printed and saved in `User-Data\logs\`. Quick visual self-test of every tool:
`App-Data\App\Saraswati.exe --tooltest`.

## Build info

| | |
|---|---|
| Version | 1.0.0 (beta) |
| Source | `Saraswati - Version 1.0.0/` at commit `52683c1` |
| Built with | MinGW-w64 GCC 13 (cross-compiled in a Linux cloud session), static, single exe |
| Verified | Runs under Wine + Mesa lavapipe: `--tooltest` scene correct, benchmark writes to `User-Data\logs` |
| Not yet verified | Real Windows PC, RTX 3090 performance, Cintiq pressure |

To build it yourself with Visual Studio instead, run `Build Saraswati.bat` in the version folder.
