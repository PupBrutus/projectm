# MilkDrop preset lab

Headless build/test/compare workspace for authoring `.milk` presets against this repo's libprojectM.

```
python milkdrop-lab/tools/lab.py build                 # configure + build preset_render.exe (Windows, VS 2022, CMake)
python milkdrop-lab/tools/lab.py check drafts/x.milk   # lint + render + verdicts + contact sheet -> out/check/sheet.png
python milkdrop-lab/tools/lab.py snap  drafts/x.milk   # save a version and diff it against the previous one
python milkdrop-lab/tools/lab.py compare a.milk b.milk # side by side + pixel diff
```

- `src/preset_render.cpp` - the renderer: hidden WGL 3.3 core context, offscreen FBO, fixed frame time,
  synthetic audio (beat/quiet/silence) or a 16-bit PCM wav, PNG captures + JSON metrics/log report.
- `tools/lab.py` - lint, verdicts (black / blown out / static / shader fallback / weak reactivity), sheets, diffs.
- `drafts/` - work in progress; `drafts/.history/` versioned snapshots.
- `out/`, `build/` - generated, gitignored.
- The workflow and lessons learned for Claude are in `.claude/skills/milkdrop-lab/SKILL.md`.

The lab sets `ENABLE_VERBOSE_LOGGING` in its own build so shader translation failures are reported.
It builds the parent tree via `add_subdirectory`, and does not touch the top-level projectM build.
