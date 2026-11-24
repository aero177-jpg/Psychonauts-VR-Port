"""Embeds release/psyvr.sample.ini in the proxy as src/common/default_ini.h, so a
first run writes a fully commented psyvr.ini rather than a bare list of keys.

    python -I tools/gen_default_ini.py          regenerate the header
    python -I tools/gen_default_ini.py --check  exit 1 if the header is stale
"""
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "..", "release", "psyvr.sample.ini")
dst = os.path.join(here, "..", "src", "common", "default_ini.h")


def render():
    with open(src, encoding="utf-8") as f:
        lines = f.read().replace("\r\n", "\n").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    out = [
        "#pragma once",
        "// Generated from release/psyvr.sample.ini by tools/gen_default_ini.py;",
        "// do not edit. Written beside the game when psyvr.ini does not exist.",
        "",
        "static const char kDefaultIni[] =",
    ]
    for line in lines:
        escaped = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append(f'    "{escaped}\\n"')
    out[-1] += ";"
    return "\n".join(out) + "\n"


text = render()
if "--check" in sys.argv:
    try:
        with open(dst, encoding="utf-8", newline="") as f:
            current = f.read().replace("\r\n", "\n")
    except OSError:
        current = ""
    if current != text:
        print("default_ini.h is out of date: run  python -I tools/gen_default_ini.py")
        sys.exit(1)
    print("default_ini.h is up to date")
else:
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote", os.path.normpath(dst))
