#!/usr/bin/env python3
"""One-time bootstrap: extract the portable Build engine functions from upstream ENGINE.C.

src/engine/parts/engine_core.c was produced by this script once and then HAND-PATCHED
(DOS/stereo code removed, overflow guards, approximate divisions, profiling hooks...).
Re-running it would discard those patches, so by default it writes next to the real file as
engine_core.gen.c for diffing. Use --force to overwrite engine_core.c itself.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cfuncs  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "upstream/SRC/ENGINE.C")
OUT = os.path.join(ROOT, "src/engine/parts/engine_core.c")

KEEP = """drawrooms scansector wallfront spritewallfront bunchfront drawalls prepwall ceilscan florscan
loadboard loadtables clipinsidebox clipinsideboxline inside getangle ksqrt krecip initksqrt
drawmasks drawmaskwall drawsprite setsprite animateoffs initspritelists insertsprite insertspritesect
insertspritestat deletesprite deletespritesect deletespritestat changespritesect changespritestat
nextsectorneighborz cansee hitscan neartag lintersect rintersect dragpoint lastwall clipmove keepaway
raytrace pushmove updatesector rotatepoint krand getzrange setview setaspect dosetaspect flushperms
rotatesprite dorotatesprite clippoly4 sectorofwall getceilzofslope getflorzofslope getzsofslope
alignceilslope alignflorslope owallmost wallmost grouscan getpalookup parascan loopnumofsector
setfirstwall preparemirror completemirror""".split()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="overwrite engine_core.c (loses hand patches)")
    ap.add_argument("-o", "--output", default=None, help="output path (default: engine_core.gen.c)")
    args = ap.parse_args()
    out = args.output or (OUT if args.force else OUT.replace("engine_core.c", "engine_core.gen.c"))
    if os.path.abspath(out) == os.path.abspath(OUT) and not args.force:
        sys.exit("refusing to overwrite %s without --force" % OUT)

    lines, funcs, order = cfuncs.parse(SRC)
    missing = [k for k in KEEP if k not in funcs]
    assert not missing, missing
    with open(out, "w") as f:
        f.write("/* Extracted verbatim from Ken Silverman's ENGINE.C by tools/gen_engine.py, then hand-patched\n"
                "   for the PS1 port. Do not regenerate without re-applying the patches. */\n\n")
        for name in order:
            if name not in KEEP:
                continue
            s, e = funcs[name]
            f.write("\n".join(lines[s : e + 1]).replace("\r", "") + "\n\n")
    print("wrote", out, len(KEEP), "functions")


if __name__ == "__main__":
    main()
