#!/usr/bin/env python3
"""MilkDrop preset lab driver: build / lint / check / sheet / compare / snap.

    python milkdrop-lab/tools/lab.py build
    python milkdrop-lab/tools/lab.py check drafts/foo.milk          # lint + render + verdicts + sheet
    python milkdrop-lab/tools/lab.py compare drafts/foo.milk presets/bar.milk
    python milkdrop-lab/tools/lab.py snap drafts/foo.milk           # save version + diff vs previous
    python milkdrop-lab/tools/lab.py sheet a.milk b.milk c.milk     # contact sheet only

Renders go through milkdrop-lab/build/Release/preset_render.exe (headless, deterministic time,
synthetic audio). Outputs land in milkdrop-lab/out/ (gitignored).
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

LAB = Path(__file__).resolve().parent.parent
REPO = LAB.parent
EXE = LAB / "build" / "Release" / "preset_render.exe"
OUT = LAB / "out"
HISTORY = LAB / "drafts" / ".history"
TEXTURES = REPO / "textures"
DEFAULT_AUDIO = ["beat", "quiet", "silence"]
NL = chr(10)


# ------------------------------------------------------------------ build
def cmd_build(_a):
    run(["cmake", "-S", str(LAB), "-B", str(LAB / "build"), "-A", "x64"])
    run(["cmake", "--build", str(LAB / "build"), "--config", "Release", "--target", "preset_render", "-j", "8"])


def run(cmd, **kw):
    print("+", " ".join(cmd))
    r = subprocess.run(cmd, **kw)
    if r.returncode:
        sys.exit(r.returncode)


def ensure_exe():
    if not EXE.exists():
        sys.exit(f"{EXE} missing - run: python milkdrop-lab/tools/lab.py build")


# ------------------------------------------------------------------ lint
# Names projectM's shader preprocessor rewrites to uniforms/swizzles; declaring a local with one breaks parsing.
RESERVED = {"time", "fps", "frame", "progress", "bass", "mid", "treb", "vol", "bass_att", "mid_att", "treb_att", "vol_att",
            "rad", "ang", "uv", "uv_orig", "aspect", "texsize", "pixsize", "rand_frame", "rand_preset", "ret",
            "roam_cos", "roam_sin", "slow_roam_cos", "slow_roam_sin", "hue_shader"} | {f"q{i}" for i in range(1, 33)}

BLOCK_RE = re.compile(r"^(per_frame_init_|per_frame_|per_pixel_|warp_|comp_|wave_\d+_per_frame|wave_\d+_per_point|"
                      r"wave_\d+_init|shape_\d+_per_frame|shape_\d+_init)(\d+)=(.*)$")


def lint(path):
    """Static checks from the skill's checklist. Returns list of (level, message)."""
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    issues = []

    def add(level, msg):
        issues.append((level, msg))

    if not any(l.startswith("MILKDROP_PRESET_VERSION=") for l in lines):
        add("error", "missing MILKDROP_PRESET_VERSION")
    if "[preset00]" not in text:
        add("error", "missing [preset00] section")
    for key in ("PSVERSION", "PSVERSION_WARP", "PSVERSION_COMP"):
        if not any(l.startswith(key + "=") for l in lines):
            add("warn", f"missing {key}")

    blocks = {}
    for i, l in enumerate(lines, 1):
        m = BLOCK_RE.match(l)
        if not m:
            continue
        prefix, n, body = m.group(1), int(m.group(2)), m.group(3)
        blocks.setdefault(prefix, []).append((n, i, body))
        is_shader = prefix in ("warp_", "comp_")
        if is_shader:
            if body and not body.startswith("`"):
                add("error", f"line {i}: shader line must start with a backtick")
            continue
        if body.count("(") != body.count(")"):
            add("error", f"line {i}: unbalanced parentheses")
        stripped = body.strip()
        if stripped and not stripped.endswith(";"):
            add("error", f"line {i}: statement does not end with ';'")
        if "==" in body:
            add("error", f"line {i}: '==' not supported, use equal()")
        if re.search(r"\b(if|for|while)\s*\(", body) and not re.search(r"\bif\s*\(", body):
            add("error", f"line {i}: loops are not supported")
        if re.search(r"\belse\b", body):
            add("error", f"line {i}: 'else' not supported, use if(c,a,b)")
    for prefix, items in blocks.items():
        nums = [n for n, _, _ in items]
        if nums != list(range(1, len(nums) + 1)):
            add("error", f"{prefix}*: numbering must be sequential from 1 (got {nums[:12]})")
    for prefix in ("warp_", "comp_"):
        if prefix in blocks:
            body = "\n".join(b for _, _, b in blocks[prefix])
            if "shader_body" not in body:
                add("error", f"{prefix}: missing shader_body")
            if not re.search(r"\bret\s*=", body):
                add("error", f"{prefix}: shader never assigns ret")
            if re.search(r"\b(for|while)\s*\(", body):
                add("warn", f"{prefix}: loops in shaders often fail translation")
            if re.search(r"#define|#include", body):
                add("error", f"{prefix}: preprocessor directives not supported")
            if re.search(r"sampler_(?!main|noise|noisevol|blur|fw|pw|fc|pc)\w+", body):
                add("warn", f"{prefix}: custom sampler - image must ship with the preset")
            if re.search(r"\?[^:]*:", body):
                add("warn", f"{prefix}: ternary operator - prefer lerp()/step()")
    for wi in range(4):
        if re.search(rf"^wavecode_{wi}_enabled=1", text, re.M):
            sm = re.search(rf"^wavecode_{wi}_smoothing=([\d.]+)", text, re.M)
            if sm and float(sm.group(1)) > 0.05 and re.search(rf"wave_{wi}_per_point.*(cos|sin)", text):
                add("info", f"wave {wi}: smoothing>0 will round off sharp corners")
    dec = re.search(r"^fDecay=([\d.]+)", text, re.M)
    if dec and float(dec.group(1)) >= 1.0:
        add("warn", "fDecay >= 1.0 - frame never fades, will blow out")
    return issues


def cmd_lint(a):
    bad = 0
    for p in a.presets:
        iss = lint(p)
        print(f"{p}: {'clean' if not iss else ''}")
        for lvl, msg in iss:
            print(f"  [{lvl}] {msg}")
            bad += lvl == "error"
    sys.exit(1 if bad else 0)


# ------------------------------------------------------------------ render
def render(presets, audios, out, duration=10.0, size="640x480", capture=None, strip=False, gain=None):
    ensure_exe()
    out.mkdir(parents=True, exist_ok=True)
    reports = {}
    for audio in audios:
        cmd = [str(EXE), "--out", str(out), "--audio", audio, "--duration", str(duration), "--size", size]
        if TEXTURES.exists():
            cmd += ["--textures", str(TEXTURES)]
        if capture:
            cmd += ["--capture", capture]
        if strip:
            cmd += ["--strip"]
        if gain:
            cmd += ["--gain", str(gain)]
        for p in presets:
            cmd += ["--preset", str(p)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        label = audio if not audio.startswith("wav:") else "wav-" + Path(audio[4:]).stem
        rp = out / f"report-{label}.json"
        if not rp.exists():
            sys.exit(f"render failed ({r.returncode}): {r.stderr or r.stdout}")
        reports[label] = json.loads(rp.read_text())
    return reports


def verdicts(report_by_audio):
    """Per-preset list of problems + summary stats."""
    res = {}
    names = [p["name"] for p in next(iter(report_by_audio.values()))["presets"]]
    for name in names:
        v = {"problems": [], "notes": [], "stats": {}}
        per = {a: next(p for p in r["presets"] if p["name"] == name) for a, r in report_by_audio.items()}
        for a, p in per.items():
            if not p.get("loaded"):
                v["problems"].append(f"[{a}] FAILED TO LOAD: {p.get('error') or 'unknown'}")
                continue
            caps = p["captures"]
            last = caps[-1] if caps else None
            v["stats"][a] = {k: p[k] for k in ("mean_luma", "luma_stddev", "mean_motion")}
            for line in p.get("log", []):
                if "Using fallback shader" in line or "Error translating" in line or "Error compiling" in line:
                    v["problems"].append(f"[{a}] SHADER FAILED, projectM used its fallback: {line.strip()[:200]}")
                elif line.startswith("[MilkdropShader] Failed preprocessed"):
                    tail = NL.join(line.rstrip().splitlines()[-16:])
                    v["notes"].append(f"[{a}] tail of the preprocessed shader that failed (macro-expanded; look for "
                                      f"odd _c3.y / _rad_ang tokens where you wrote a name):{NL}{tail}")
                elif line.startswith("[MilkdropShader] Failed"):
                    pass
                elif "GLInfo" not in line:
                    v["notes"].append(f"[{a}] log: {line.strip()[:400]}")
            if last and a != "silence":
                if p["mean_luma"] < 0.02 or last["black"] > 0.97:
                    v["problems"].append(f"[{a}] renders (nearly) BLACK (luma {p['mean_luma']:.3f})")
                if last["clipped"] > 0.35 or p["mean_luma"] > 0.75:
                    v["problems"].append(f"[{a}] BLOWN OUT (clipped {last['clipped']:.0%}, luma {p['mean_luma']:.2f})")
                if p["mean_motion"] < 0.002 and p["mean_luma"] >= 0.02:
                    v["problems"].append(f"[{a}] STATIC (frame-to-frame motion {p['mean_motion']:.4f})")
                if last["contrast"] < 0.03 and p["mean_luma"] >= 0.02:
                    v["notes"].append(f"[{a}] flat image (contrast {last['contrast']:.3f})")
        if "beat" in per and "silence" in per and per["beat"].get("loaded") and per["silence"].get("loaded"):
            b, s = per["beat"], per["silence"]
            d = abs(b["mean_motion"] - s["mean_motion"]) + abs(b["luma_stddev"] - s["luma_stddev"])
            v["stats"]["reactivity"] = round(d, 4)
            if d < 0.004:
                v["notes"].append("audio reactivity looks WEAK: beat vs silence renders barely differ")
        res[name] = v
    return res


# ------------------------------------------------------------------ sheets
def load(p, scale=1.0):
    im = Image.open(p).convert("RGB")
    if scale != 1.0:
        im = im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS)
    return im


def make_sheet(rows, out_path, col_labels=None, cell_w=320):
    """rows: list of (row_label, [image paths])."""
    if not rows:
        return
    first = load(rows[0][1][0])
    scale = cell_w / first.width
    cw, ch = cell_w, int(first.height * scale)
    ncols = max(len(r[1]) for r in rows)
    lab_w, head = 190, (18 if col_labels else 0)
    sheet = Image.new("RGB", (lab_w + ncols * cw, head + len(rows) * ch), (18, 18, 18))
    d = ImageDraw.Draw(sheet)
    for ci, cl in enumerate(col_labels or []):
        d.text((lab_w + ci * cw + 4, 3), cl, fill=(200, 200, 200))
    for ri, (label, paths) in enumerate(rows):
        y = head + ri * ch
        d.text((4, y + 4), label[:30] + ("\n" + label[30:60] if len(label) > 30 else ""), fill=(230, 230, 230))
        for ci, p in enumerate(paths):
            sheet.paste(load(p, scale), (lab_w + ci * cw, y))
    sheet.save(out_path)
    return out_path


def diff_image(a, b, out_path):
    A = np.asarray(load(a), dtype=np.int16)
    B = np.asarray(load(b), dtype=np.int16)
    if A.shape != B.shape:
        B = np.asarray(load(b).resize((A.shape[1], A.shape[0])), dtype=np.int16)
    d = np.abs(A - B).sum(axis=2) / 3.0
    Image.fromarray(np.clip(d * 3, 0, 255).astype(np.uint8)).save(out_path)
    return float(d.mean() / 255.0)


def caps_of(report, name, audio):
    p = next(p for p in report[audio]["presets"] if p["name"] == name)
    return [c["png"] for c in p.get("captures", [])]


# ------------------------------------------------------------------ commands
def stem_key(p):
    return re.sub(r"[^A-Za-z0-9._-]", "_", Path(p).stem)


def print_verdicts(vs):
    ok = True
    for name, v in vs.items():
        status = "PASS" if not v["problems"] else "FAIL"
        ok &= status == "PASS"
        st = v["stats"].get("beat") or next((x for k, x in v["stats"].items() if isinstance(x, dict)), {})
        stats = f"luma {st.get('mean_luma', 0):.2f} motion {st.get('mean_motion', 0):.3f} flicker {st.get('luma_stddev', 0):.3f}"
        if "reactivity" in v["stats"]:
            stats += f" reactivity {v['stats']['reactivity']}"
        print(f"[{status}] {name}   ({stats})")
        for p in v["problems"]:
            print("   problem:", p)
        for n in v["notes"][:6]:
            print("   note:   ", n)
    return ok


def cmd_check(a):
    ok = True
    for p in a.presets:
        for lvl, msg in lint(p):
            print(f"lint [{lvl}] {Path(p).name}: {msg}")
            ok &= lvl != "error"
    out = OUT / (a.tag or "check")
    if out.exists() and not a.keep:
        shutil.rmtree(out)
    audios = a.audio.split(",")
    rep = render(a.presets, audios, out, a.duration, a.size, a.capture)
    vs = verdicts(rep)
    ok &= print_verdicts(vs)
    rows = []
    for p in a.presets:
        name = stem_key(p)
        for aud in rep:
            pr = next(x for x in rep[aud]["presets"] if x["name"] == name)
            if pr.get("loaded"):
                rows.append((f"{name} [{aud}]", caps_of(rep, name, aud)))
    if rows:
        labels = [f"t={c}" for c in next(iter(rep.values()))["presets"][0].get("captures", [])] and \
                 [f"t={c['t']:.1f}s" for c in next(x for x in next(iter(rep.values()))["presets"] if x.get("loaded"))["captures"]]
        sheet = make_sheet(rows, out / "sheet.png", labels)
        print("sheet:", sheet)
    (out / "verdicts.json").write_text(json.dumps(vs, indent=1))
    sys.exit(0 if ok else 1)


def cmd_sheet(a):
    out = OUT / (a.tag or "sheet")
    out.mkdir(parents=True, exist_ok=True)
    rep = render(a.presets, [a.audio], out, a.duration, a.size, a.capture)
    rows = []
    for p in a.presets:
        name = stem_key(p)
        pr = next(x for x in rep[next(iter(rep))]["presets"] if x["name"] == name)
        if pr.get("loaded"):
            rows.append((name, caps_of(rep, name, next(iter(rep)))))
        else:
            print(f"{name}: FAILED - {pr.get('error')}")
    print("sheet:", make_sheet(rows, out / "sheet.png"))


def cmd_compare(a):
    """Side by side of A vs B: same audio, same timestamps, plus pixel-diff row and metrics table."""
    out = OUT / (a.tag or "compare")
    if out.exists():
        shutil.rmtree(out)
    a_path, b_path = a.a, a.b
    rep = render([a_path, b_path], [a.audio], out, a.duration, a.size, a.capture)
    label = next(iter(rep))
    ka, kb = stem_key(a_path), stem_key(b_path)
    if ka == kb:
        sys.exit("A and B share a file stem; rename one so their outputs don't collide")
    pa, pb = (next(x for x in rep[label]["presets"] if x["name"] == k) for k in (ka, kb))
    for k, p in ((ka, pa), (kb, pb)):
        if not p.get("loaded"):
            print(f"{k}: FAILED TO LOAD - {p.get('error')}")
    if not (pa.get("loaded") and pb.get("loaded")):
        sys.exit(1)
    ca, cb = caps_of(rep, ka, label), caps_of(rep, kb, label)
    diffs = []
    for i, (x, y) in enumerate(zip(ca, cb)):
        dp = out / f"diff_{i}.png"
        diffs.append(dp)
        pa["captures"][i]["diff"] = diff_image(x, y, dp)
    labels = [f"t={c['t']:.1f}s" for c in pa["captures"]]
    sheet = make_sheet([(f"A {ka}", ca), (f"B {kb}", cb), ("diff x3", [str(d) for d in diffs])], out / "compare.png", labels)
    print(f"{'metric':<14}{'A':>10}{'B':>10}")
    for m in ("mean_luma", "luma_stddev", "mean_motion"):
        print(f"{m:<14}{pa[m]:>10.4f}{pb[m]:>10.4f}")
    for m in ("contrast", "sat", "clipped", "black"):
        va = np.mean([c[m] for c in pa["captures"]])
        vb = np.mean([c[m] for c in pb["captures"]])
        print(f"{m:<14}{va:>10.4f}{vb:>10.4f}")
    print("mean pixel diff per capture:", ", ".join(f"{c['diff']:.3f}" for c in pa["captures"]))
    print("sheet:", sheet)


def cmd_snap(a):
    """Store the preset as the next version, check it, and compare with the previous version."""
    src = Path(a.preset)
    hist = HISTORY / src.stem
    hist.mkdir(parents=True, exist_ok=True)
    versions = sorted(hist.glob("v*.milk"))
    n = len(versions) + 1
    dst = hist / f"v{n:03d}.milk"
    shutil.copy2(src, dst)
    print(f"saved {dst.relative_to(REPO)}")
    if versions:
        shutil.copy2(versions[-1], hist / f"v{n-1:03d}_{src.stem}.milk")
        ns = argparse.Namespace(a=str(hist / f"v{n-1:03d}_{src.stem}.milk"), b=str(dst), audio="beat",
                                duration=a.duration, size=a.size, capture=a.capture, tag=f"snap-{src.stem}")
        try:
            cmd_compare(ns)
        finally:
            (hist / f"v{n-1:03d}_{src.stem}.milk").unlink(missing_ok=True)
    else:
        print("first version; nothing to compare")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, audio_default="beat"):
        p.add_argument("--audio", default=audio_default, help="beat|quiet|silence|wav:PATH (check accepts a comma list)")
        p.add_argument("--duration", type=float, default=10.0)
        p.add_argument("--size", default="640x480")
        p.add_argument("--capture", default="1,3,6,10", help="capture times in seconds")
        p.add_argument("--tag", help="output subfolder under out/")

    sub.add_parser("build").set_defaults(fn=cmd_build)
    p = sub.add_parser("lint"); p.add_argument("presets", nargs="+"); p.set_defaults(fn=cmd_lint)
    p = sub.add_parser("check"); p.add_argument("presets", nargs="+"); common(p, ",".join(DEFAULT_AUDIO))
    p.add_argument("--keep", action="store_true"); p.set_defaults(fn=cmd_check)
    p = sub.add_parser("sheet"); p.add_argument("presets", nargs="+"); common(p); p.set_defaults(fn=cmd_sheet)
    p = sub.add_parser("compare"); p.add_argument("a"); p.add_argument("b"); common(p); p.set_defaults(fn=cmd_compare)
    p = sub.add_parser("snap"); p.add_argument("preset"); common(p); p.set_defaults(fn=cmd_snap)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
