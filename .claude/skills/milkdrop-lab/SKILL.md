---
name: milkdrop-lab
description: Use when authoring, debugging or tuning MilkDrop (.milk) presets in this repo and you need to actually render them - build/lint/render/compare loop via milkdrop-lab/tools/lab.py. Pair with milkdrop-preset-authoring for the file format and recipes.
---

# MilkDrop preset lab

`milkdrop-lab/` renders presets headlessly through this repo's libprojectM (deterministic time, synthetic
audio, no window shown) so you can SEE the result instead of guessing. Never hand off a preset you have not
rendered with `check`. Recipes and file format live in the `milkdrop-preset-authoring` skill; this skill is the
test loop plus corrections found by using it.

## Layout
- `milkdrop-lab/drafts/*.milk` - work in progress. `drafts/.history/<name>/vNNN.milk` = snapshots.
- `milkdrop-lab/out/` - renders, sheets, JSON reports (gitignored). `milkdrop-lab/build/` gitignored.
- `presets/` - the user's large library (untracked); use as reference/baseline for compare, do not edit.
- Finished presets: copy from drafts/ to `presets/` only when the user asks.

## Commands (run from repo root; Python 3 + Pillow + numpy)
```
python milkdrop-lab/tools/lab.py build                       # once, or after C++ changes (cmake, VS 2022)
python milkdrop-lab/tools/lab.py lint  drafts/x.milk         # static checks only, instant
python milkdrop-lab/tools/lab.py check drafts/x.milk         # lint + render beat/quiet/silence + verdicts + sheet
python milkdrop-lab/tools/lab.py snap  drafts/x.milk         # save vNNN, render, compare against previous version
python milkdrop-lab/tools/lab.py compare A.milk B.milk       # side-by-side + pixel diff + metric table
python milkdrop-lab/tools/lab.py sheet a.milk b.milk ...     # contact sheet of several presets (e.g. a whole set)
```
Options: `--audio beat|quiet|silence|wav:PATH` (check takes a comma list), `--capture 1,3,6,10` (seconds),
`--duration`, `--size 640x480`, `--tag NAME` (output folder `out/NAME`). Preset filenames must have unique stems.
The raw tool is `milkdrop-lab/build/Release/preset_render.exe` (`--verbose` keeps all projectM log lines).

## Loop
1. Write/edit `drafts/<name>.milk`.
2. `check` -> read the printed verdicts, then LOOK at `out/check/sheet.png` with the Read tool (rows = audio
   conditions, columns = timestamps). Verdicts catch: load failure, shader fallback, black, blown out, static,
   weak audio reactivity. They do not judge aesthetics or orientation - only your eyes on the sheet do.
3. Change ONE thing, `snap`, read the compare sheet (row A = previous, B = new, third row = pixel diff).
4. Iterate until `check` is PASS and the sheet matches the concept. Also review a few `--capture` points
   after 20-30 s if the preset has slow accumulation (`--duration 30 --capture 5,15,30`).
5. For a themed set, `sheet` all of them together to compare palette/brightness consistency.

Interpreting metrics: luma ~0.05-0.35 is healthy; `black` fraction > 0.9 with luma < 0.03 means nearly empty
(fine for sparse line art, wrong for fields). `reactivity` < 0.004 means beat and silence look the same.
Synthetic `beat` = 120 bpm kick + hat + snare + bass; `quiet` is the same at 15% level (tests bass_att-based
thresholds); real music: `--audio wav:file.wav` (16-bit PCM only).

## Corrections to milkdrop-preset-authoring (found with this lab, all verified by rendering)
1. **Never name a shader local `mid`, `bass`, `treb`, `time`, `rad`, `ang`, `q1..q32` etc.** projectM macro-expands
   these before HLSL parsing (`mid` -> `_c3.y`), so `float3 mid = ...` fails, projectM logs "Using fallback
   shader" and the preset renders with the default comp shader. The skill's comp template used `mid`; use
   `midc`. `lab.py lint` flags this; `check` reports SHADER FAILED.
2. **`aspectx`/`aspecty` are NOT defined inside custom wave / shape code** (they read as 0). The skill's pentagram
   `x = 0.5 + rr*cos(a)*aspecty` collapses to a vertical line. Set `q3 = aspecty;` in per_frame and use `q3`.
3. **Avoid `a`, `r`, `g`, `b`, `x`, `y` as scratch names in wave per-point code**; they are the output variables (unverified hazard, not the cause of the line bug).
   Use `th`, `k`, `rr`, etc. (Renaming `a` alone did not fix the line, but it is still a hazard.)

## Gotchas
- Time is deterministic (`projectm_set_frame_time`), so renders are reproducible for the same audio mode.
  The noise textures/random values seeded per preset load may still differ slightly between runs.
- A preset that renders black can be a *load-time* problem: read `out/<tag>/report-beat.json` `log` for the
  full projectM message, including the failing shader source tail.
- The harness needs an OpenGL 3.3 core context (uses a hidden Win32 window); it will not run over plain RDP
  sessions without GPU acceleration.
