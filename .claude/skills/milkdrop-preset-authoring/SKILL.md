---
name: "milkdrop-preset-authoring"
description: "Use when writing, editing or debugging MilkDrop (.milk) visualizer presets for projectM, including themed sets like Halloween."
---

# MilkDrop preset authoring for projectM

Goal: produce `.milk` files that load cleanly in projectM 4.x, react to music, and look intentional. projectM parses the equations with projectm-eval and translates the HLSL shader blocks to GLSL on the fly, so anything exotic in the shaders is the #1 cause of a preset being silently skipped.

## Pipeline (follow in order)

1. **Concept** - one sentence: the image, the motion, what the beat does. e.g. "Inverted pentagram glowing orange, slowly rotating, flares on kick drums over a dark smoky red trail."
2. **Palette** - pick 2-4 colors as RGB 0-1 (see Halloween palette below). Decide the background: dark presets need low decay and/or a darkening comp shader.
3. **Motion field** - choose base warp params (zoom, rot, dx/dy, warp) then per-pixel tweaks. This is the "feedback" that creates trails, smoke, bubbling.
4. **Foreground** - what draws new pixels each frame: the built-in wave, custom waves (lines/stars/glyphs), custom shapes (circles/polygons/orbs).
5. **Audio reactivity** - map `bass`, `mid`, `treb` (and smoothed `_att` versions) to a few parameters. Less is more: 2-3 mappings read better than 10.
6. **Shaders** - warp shader for distortion/smoke, comp shader for final color grade. Keep them short (see projectM-safe rules).
7. **Write the file** from the template below. Every numbered equation line must be sequential with no gaps.
8. **Lint** with the checklist at the bottom.
9. **Test in projectM** - in the projectm repo, use the `milkdrop-lab` skill (`python milkdrop-lab/tools/lab.py check drafts/x.milk`) to render headlessly and read the contact sheet; a shader that silently fell back is reported as SHADER FAILED. Otherwise drop into the presets folder, lock the preset, play a quiet track and a loud one. If projectM skips it or it renders black, bisect: delete the comp shader, then the warp shader, then custom waves, until it loads.
10. **Tune** - adjust ranges so it never goes fully white (blown out) or fully black for more than a moment.

## File structure

```
MILKDROP_PRESET_VERSION=201
PSVERSION=2
PSVERSION_WARP=2
PSVERSION_COMP=2
[preset00]
fRating=5.000
fGammaAdj=1.500
fDecay=0.950
fVideoEchoZoom=1.000
fVideoEchoAlpha=0.000
nVideoEchoOrientation=0
nWaveMode=0
bAdditiveWaves=1
bWaveDots=0
bWaveThick=1
bModWaveAlphaByVolume=0
bMaximizeWaveColor=0
bTexWrap=1
bDarkenCenter=0
bRedBlueStereo=0
bBrighten=0
bDarken=0
bSolarize=0
bInvert=0
fWaveAlpha=0.000
fWaveScale=1.000
fWaveSmoothing=0.750
fWaveParam=0.000
fModWaveAlphaStart=0.750
fModWaveAlphaEnd=0.950
fWarpAnimSpeed=1.000
fWarpScale=1.000
fZoomExponent=1.000
fShader=0.000
zoom=1.000
rot=0.000
cx=0.500
cy=0.500
dx=0.000
dy=0.000
warp=0.010
sx=1.000
sy=1.000
wave_r=1.000
wave_g=0.450
wave_b=0.000
wave_x=0.500
wave_y=0.500
ob_size=0.000
ob_r=0.000
ob_g=0.000
ob_b=0.000
ob_a=0.000
ib_size=0.000
ib_r=0.000
ib_g=0.000
ib_b=0.000
ib_a=0.000
nMotionVectorsX=0.000
nMotionVectorsY=0.000
mv_a=0.000
b1n=0.000
b2n=0.000
b3n=0.000
b1x=1.000
b2x=1.000
b3x=1.000
b1ed=0.250

(custom waves, custom shapes, equations, shaders follow)
```

Section order that works everywhere: header params -> `wavecode_N_*` + `wave_N_*` blocks -> `shapecode_N_*` + `shape_N_*` blocks -> `per_frame_init_N` -> `per_frame_N` -> `per_pixel_N` -> `warp_N` -> `comp_N`.

## Equation language (per_frame / per_pixel / wave / shape)

- C-like expressions, each statement ends in `;`. One or more statements per numbered line: `per_frame_1=zoom = 1 + 0.02*bass;`
- Functions: `sin cos tan asin acos atan atan2 sqr sqrt pow abs min max sign rand(n) int above(a,b) below(a,b) equal(a,b) if(cond,a,b) bor band bnot sigmoid exp log log10`.
- No `if` statements or loops - use `if(cond, a, b)` and `above/below`. Booleans are 0/1.
- Useful inputs: `time`, `frame`, `fps`, `bass mid treb`, `bass_att mid_att treb_att` (smoothed; about 1.0 on average, spikes to 1.5-3 on hits), `aspectx aspecty`, `progress`.
- Per-frame outputs: `zoom rot cx cy dx dy warp sx sy decay wave_r/g/b/a wave_x wave_y ob_* ib_* echo_zoom echo_alpha gamma` and `q1..q32` (passed to per-pixel, waves, shapes and shaders).
- Per-pixel inputs `x y rad ang` (x,y in 0-1; rad 0 at center to ~0.7 at corners; ang in radians). Outputs the same motion vars as per-frame, per mesh point.
- Beat detection pattern (in per_frame): `is_beat = above(bass_att, 1.3)*above(time, last_beat + 0.25); last_beat = if(is_beat, time, last_beat); q1 = is_beat;`
- Initialize persistent vars in `per_frame_init_1=` so they don't start at garbage.

## Custom waves (lines, stars, glyphs)

Up to 4 (`wavecode_0..3`). Key fields:
```
wavecode_0_enabled=1
wavecode_0_samples=6
wavecode_0_sep=0
wavecode_0_bSpectrum=0
wavecode_0_bUseDots=0
wavecode_0_bDrawThick=1
wavecode_0_bAdditive=1
wavecode_0_scaling=1.000
wavecode_0_smoothing=0.000
wavecode_0_r=1.000
wavecode_0_g=0.300
wavecode_0_b=0.000
wavecode_0_a=1.000
wave_0_per_frame1=...
wave_0_per_point1=...
```
Per-point gets `sample` (0 to 1 across the samples, inclusive), `value1 value2` (audio), and sets `x y r g b a`. Set `smoothing=0` for anything with sharp corners, or the shape rounds off.

### Pentagram (5-point star drawn in one stroke)
Visit star vertices in order 0,2,4,1,3,0 = step 4*pi/5 per point, 6 samples total.
```
wavecode_0_samples=6
wave_0_per_point1=k = int(sample*5 + 0.5);
wave_0_per_point2=th = k*4*3.14159/5 + q2 + 1.5708;
wave_0_per_point3=rr = 0.3 + 0.05*q1;
wave_0_per_point4=x = 0.5 + rr*cos(th)*q3;
wave_0_per_point5=y = 0.5 + rr*sin(th);
```
`q2` = rotation (e.g. `q2 = time*0.2;`), `q1` = beat pulse, `q3` = aspect: set `q3 = aspecty;` in per_frame. **`aspectx`/`aspecty` are undefined (read as 0) inside custom wave and shape code**, so using them there collapses x to 0.5 and draws a vertical line (verified by rendering). Also avoid `a r g b x y` as scratch names in wave code, since they are output variables. `+1.5708` vs `-1.5708` flips point-up vs point-down; verify orientation on screen since y direction can differ. Add a circle around it with a custom shape (below) with `sides=40`, `thickOutline=1`, fill alpha 0, border alpha 1. Draw the star twice (wave 0 thick dim, wave 1 thin bright) for a glow.

## Custom shapes (circles, polygons, orbs)

Up to 4 (`shapecode_0..3`), each can draw `num_inst` copies (use `instance` in per-frame to spread them).
```
shapecode_0_enabled=1
shapecode_0_sides=40
shapecode_0_additive=1
shapecode_0_thickOutline=1
shapecode_0_textured=0
shapecode_0_num_inst=1
shapecode_0_x=0.500
shapecode_0_y=0.500
shapecode_0_rad=0.350
shapecode_0_ang=0.000
shapecode_0_tex_ang=0.000
shapecode_0_tex_zoom=1.000
shapecode_0_r=0.000
shapecode_0_g=0.000
shapecode_0_b=0.000
shapecode_0_a=0.000
shapecode_0_r2=0.000
shapecode_0_g2=0.000
shapecode_0_b2=0.000
shapecode_0_a2=0.000
shapecode_0_border_r=1.000
shapecode_0_border_g=0.300
shapecode_0_border_b=0.000
shapecode_0_border_a=1.000
shape_0_per_frame1=rad = 0.35 + 0.03*q1;
```
`r,g,b,a` = center color, `r2..a2` = edge color (radial gradient). Bubbles: `num_inst=12`, per-frame positions from `instance` + `time`, low alpha center, bright edge.

## Shaders (warp_ and comp_)

Each line is prefixed with a backtick after the `=`. Shape:
```
warp_1=`shader_body
warp_2=`{
warp_3=`    float2 d = (tex2D(sampler_noise_lq, uv*0.5 + time*0.02).xy - 0.5)*0.004;
warp_4=`    ret = tex2D(sampler_main, uv + d).rgb;
warp_5=`    ret *= 0.97;
warp_6=`}
```
- `ret` is a `float3` and must be assigned.
- Available: `uv`, `uv_orig`, `rad`, `ang`, `time`, `bass mid treb` and `_att`, `q1..q32`, `texsize` (xy = size, zw = 1/size), `aspect`.
- Samplers: `sampler_main` (previous frame), `sampler_noise_lq/mq/hq` (2D noise), `sampler_noisevol_lq/hq` (3D), blur via `GetBlur1(uv)`, `GetBlur2`, `GetBlur3` (these only work if the blur is actually used; keep `b1x` etc. at 1).
- **Warp shader** decides how the previous frame is re-sampled (smoke, drips, melt). **Comp shader** decides final screen color and is NOT fed back, so color grading here never smears.

### projectM-safe HLSL rules
- Use float literals with a decimal (`1.0`, `0.5`), declare types explicitly (`float3 c = ...`).
- Stick to common intrinsics: `tex2D tex3D lerp saturate sin cos abs frac floor pow dot length normalize min max smoothstep clamp`.
- No custom image textures (`sampler_myimage`) unless the image ships with the preset; missing ones fail to load.
- No loops, no user functions, no `#define`, no arrays. Avoid vector ternaries (`a ? v1 : v2` with float3) - use `lerp(v2, v1, step)`.
- Swizzles fine (`.rgb`, `.xy`). Assign every variable before use.
- **Never name a local variable after a built-in**: `mid bass treb vol time fps frame progress rad ang uv aspect texsize ret q1..q32` (and `_att` forms). projectM macro-expands these before HLSL parsing (`mid` becomes `_c3.y`), so `float3 mid = ...` fails to parse, projectM logs "Using fallback shader", and the preset silently renders with the default shader. Use `midc`, `col_mid`, etc.
- If a preset gets skipped, remove the shaders first; if it then loads, the translator choked on something - simplify line by line.

### Comp shader color-grade template (dark + palette mapping)
```
comp_1=`shader_body
comp_2=`{
comp_3=`    float3 c = tex2D(sampler_main, uv).rgb;
comp_4=`    float l = dot(c, float3(0.30, 0.59, 0.11));
comp_5=`    float3 dark = float3(0.02, 0.0, 0.04);
comp_6=`    float3 midc = float3(0.60, 0.0, 0.05);
comp_7=`    float3 hot  = float3(1.00, 0.55, 0.05);
comp_8=`    float3 g = lerp(dark, midc, saturate(l*2.0));
comp_9=`    g = lerp(g, hot, saturate(l*2.0 - 1.0));
comp_10=`    g += GetBlur1(uv).rgb * 0.4;
comp_11=`    ret = g * (1.0 - rad*0.6);
comp_12=`}
```
(Remove line 10 if blur causes problems.) The last line is a vignette.

## Halloween palette + recipes

| Name | RGB |
|---|---|
| Pumpkin | 1.00, 0.45, 0.00 |
| Blood | 0.55, 0.00, 0.03 |
| Toxic slime | 0.40, 1.00, 0.10 |
| Witch purple | 0.35, 0.00, 0.55 |
| Bone | 0.90, 0.88, 0.75 |
| Void | 0.02, 0.00, 0.03 |

- **Summoning circle**: pentagram custom wave + circle shape, `decay 0.92`, slow `rot`, zoom 0.99 so trails fall inward, comp grade void->blood->pumpkin, flare `rad` on beat.
- **Bubbling cauldron**: per-pixel `dy = -0.003 - 0.004*bass_att*below(y,0.6);` plus `zoom = 1 + 0.01*sin(x*25 + time*3)*sin(y*20 - time*4);`, bubble shapes rising, comp grade void->witch purple->toxic slime.
- **Blood drip**: per-pixel `dy = 0.004*(0.5 + 0.5*sin(x*40))*(1 + bass_att);` (drips down; flip sign if they go up), warp shader adds noise offset, comp grade to blood with bone-white highlights.
- **Dark psychedelia**: high `warp` (0.5-1.5) driven by `mid_att`, `rot = 0.02*sin(time*0.3)`, comp shader hue-cycles between purple and slime using `sin(time + l*6.0)`, darken hard with `ret *= l*1.5`.

## Lint checklist (run before handing off)

- [ ] Header has `MILKDROP_PRESET_VERSION=201`, `PSVERSION*=2` and `[preset00]`.
- [ ] Every equation block numbers from 1 with no gaps (`per_frame_1, _2, _3...`, `warp_1, _2...`).
- [ ] Every equation statement ends with `;`. Parentheses balance on each line.
- [ ] No `if` statements, loops, or `==` (use `equal()`).
- [ ] Every shader line starts with a backtick; block is `shader_body` then `{ ... }` and assigns `ret`.
- [ ] Shader uses only the safe intrinsic list; floats have decimals.
- [ ] Custom waves with corners have `smoothing=0.000`.
- [ ] Brightness bounded: decay < 1, comp shader or gamma keeps peaks from clipping.
- [ ] Filename is descriptive and unique, e.g. `Halloween - Summoning Circle.milk`.
- [ ] Tested in projectM at quiet and loud volume; it does not get skipped.

## Deliverable
Write each preset as its own `.milk` file, describe in one line what it looks like and what the beat does, and list anything that should be verified on screen (orientation, brightness).