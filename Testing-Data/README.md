# Testing-Data

Shared test fixtures for all Saraswati versions (not a version folder, never
copied forward). Copy a fixture into a version's `User-Data/` only for the
duration of a test, then delete the copy again — see CLAUDE.md.

| Dataset | Purpose |
|---|---|
| `import-formats/` | Small images (PNG with alpha, JPEG, WebP lossless, animated GIF, SVG) for testing *File > Open* / *Import image as layer* and `--import`. The GIF has 2 frames (only the first is imported). |

PSD round-trips are tested without a fixture: `--demo --save <file.psd>` writes one, then `--open <file.psd>` reads it back.
