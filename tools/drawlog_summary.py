"""Summarise draw logs (<name>.draws.jsonl, written by the shim's `drawlog` /
`shot` command; tools/README.md) as Markdown tables.

    python -I tools/drawlog_summary.py <file.jsonl> [more files]

Table 1 groups draws by (vs hash, ps hash, vertex declaration or FVF, call
type, kind). Table 2 lists the SetScissorRect callers behind the draws that
ran with the scissor test on. Standard library only.
"""
import json
import sys
from collections import Counter, OrderedDict

USAGE = ["POS", "BLENDW", "BLENDI", "NRM", "PSIZE", "TEX", "TAN", "BIN",
         "TESS", "POSN", "COL", "FOG", "DEPTH", "SAMPLE"]
CALLS = {"DP": "DP", "DIP": "DIP", "DPUP": "DPUP", "DIPUP": "DIPUP"}


def decl_signature(d):
    if "fvf" in d:
        return "fvf " + str(d["fvf"])
    decl = d.get("decl")
    if not decl:
        return "none"
    parts = []
    for e in decl:
        u = e.get("usage", -1)
        name = "POSITIONT" if u == 9 else (USAGE[u] if 0 <= u < len(USAGE) else "U%d" % u)
        s = "%s%d:%d@%d" % (name, e.get("index", 0), e.get("type", -1), e.get("offset", 0))
        if e.get("stream", 0):
            s = "s%d." % e["stream"] + s
        parts.append(s)
    return ",".join(parts)


def cell(s):
    if s is None:
        return "-"
    return str(s).replace("|", "/").replace("\n", " ")


def top(counter, n):
    return ", ".join("%s x%d" % (k, v) for k, v in counter.most_common(n))


def fmt_row(v):
    return "[" + " ".join(("%.4g" % x) if x is not None else "nan" for x in v) + "]"


class Group:
    def __init__(self):
        self.count = 0
        self.pretransformed = False
        self.dynamic = Counter()
        self.targets = Counter()
        self.scissor_on = 0
        self.rects = Counter()
        self.callers = Counter()
        self.screen = OrderedDict()     # reg -> distinct v0 rows
        self.rt_tex = Counter()


def summarise(path):
    shaders = {}
    draws = []
    frame = None
    with open(path, encoding="ascii", errors="replace") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except ValueError as e:
                print("%s:%d: bad json: %s" % (path, n, e), file=sys.stderr)
                continue
            t = r.get("t")
            if t == "frame":
                frame = r
            elif t == "shader":
                shaders[(r["kind"], r["hash"])] = r
            elif t == "draw":
                draws.append(r)

    print("## %s" % path)
    print()
    if frame:
        print("Frame %s, back buffer %s, %s draws logged%s." % (
            frame.get("frame"), "x".join(str(x) for x in frame.get("back", [])),
            frame.get("draws"), (", %s dropped" % frame["dropped"]) if frame.get("dropped") else ""))
        print()

    groups = OrderedDict()
    scissor_callers = OrderedDict()
    for d in draws:
        key = (d.get("vs"), d.get("ps"), decl_signature(d), d.get("call"), d.get("kind"))
        g = groups.setdefault(key, Group())
        g.count += 1
        g.pretransformed = g.pretransformed or bool(d.get("pretransformed"))
        s0 = d.get("stream0")
        if s0:
            g.dynamic["dyn" if s0.get("dynamic") else "static"] += 1
        rt = d.get("rt0")
        if rt:
            g.targets["%dx%d" % (rt.get("w", 0), rt.get("h", 0))] += 1
        sc = d.get("scissor") or {}
        if sc.get("on"):
            g.scissor_on += 1
            rect = tuple(sc.get("rect", []))
            g.rects[",".join(str(x) for x in rect)] += 1
            ck = (sc.get("caller"), rect)
            scissor_callers[ck] = scissor_callers.get(ck, 0) + 1
        g.callers[d.get("caller")] += 1
        for e in d.get("vsc", []):
            reg = e.get("reg")
            if reg in (16, 17, 18, 19) and e.get("v0"):
                rows = g.screen.setdefault(reg, OrderedDict())
                rows[fmt_row(e["v0"])] = True
        for t in d.get("tex", []):
            if t.get("rt"):
                g.rt_tex["s%d %dx%d" % (t.get("stage", 0), t.get("w", 0), t.get("h", 0))] += 1

    print("### Draw groups")
    print()
    print("| # | n | call | vs | ps | decl | kind | pretransformed | vb | render targets | "
          "scissor on | scissor rects | callers (top 3) | ScreenMatrix v0 | rt textures | ctab |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    ordered = sorted(groups.items(), key=lambda kv: -kv[1].count)
    for i, (key, g) in enumerate(ordered, 1):
        vs, ps, sig, call, kind = key
        vs_sh = shaders.get(("vs", vs)) if vs else None
        ps_sh = shaders.get(("ps", ps)) if ps else None
        names = []
        has_sm = False
        for sh in (vs_sh, ps_sh):
            if sh and sh.get("ctab"):
                for c in sh["ctab"]:
                    if c["name"] == "ScreenMatrix":
                        has_sm = True
                    if c["name"] not in names:
                        names.append(c["name"])
        ctab = ",".join(names)
        if len(ctab) > 70:
            ctab = ctab[:67] + "..."
        sm = ""
        if has_sm:
            sm = "; ".join("c%d=%s" % (reg, " | ".join(list(rows)[:3]))
                           for reg, rows in sorted(g.screen.items()))
        print("| %d | %d | %s | %s | %s | %s | %s | %s | %s | %s | %d | %s | %s | %s | %s | %s |" % (
            i, g.count, cell(call), cell(vs), cell(ps), cell(sig), cell(kind),
            "yes" if g.pretransformed else "no",
            cell(top(g.dynamic, 2) or "up"),
            cell(top(g.targets, 4)),
            g.scissor_on,
            cell(top(g.rects, 3)) if g.rects else "",
            cell(top(g.callers, 3)),
            cell(sm), cell(top(g.rt_tex, 3)), cell(ctab)))
    print()

    print("### SetScissorRect callers (draws with the scissor test on)")
    print()
    print("| caller | rect l,t,r,b | draws |")
    print("|---|---|---|")
    for (caller, rect), n in sorted(scissor_callers.items(), key=lambda kv: -kv[1]):
        print("| %s | %s | %d |" % (cell(caller), ",".join(str(x) for x in rect), n))
    if not scissor_callers:
        print("| (none) | | 0 |")
    print()


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for path in argv[1:]:
        summarise(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
