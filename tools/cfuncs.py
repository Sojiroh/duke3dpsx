#!/usr/bin/env python3
"""Crude function extractor for the old K&R-style Build/Duke sources.

A function starts at a column-0 line that looks like `name(` or `type name(` and whose
body opens with a column-0 `{` within the next 3 lines; it ends at the next column-0 `}`.
"""
import re
import sys

HDR = re.compile(r"^(?:(?:static|unsigned|signed|long|short|char|int|void|spritetype|sectortype|walltype|interrupt)\s+)*[\*]*([A-Za-z_]\w*)\s*\(")


def parse(path):
    lines = open(path, encoding="latin-1").read().split("\n")
    funcs = {}
    order = []
    i = 0
    n = len(lines)
    while i < n:
        m = HDR.match(lines[i])
        if m and not lines[i].rstrip().endswith(";") and not lines[i].startswith("#"):
            # find '{' at column 0 within next 3 lines (or end of this line)
            j = i
            found = None
            while j < min(i + 4, n):
                if lines[j].startswith("{"):
                    found = j
                    break
                if j > i and (lines[j].rstrip().endswith(";") or HDR.match(lines[j])):
                    break
                j += 1
            if found is not None:
                k = found + 1
                while k < n and not lines[k].startswith("}"):
                    k += 1
                name = m.group(1)
                funcs[name] = (i, k)  # 0-based inclusive
                order.append(name)
                i = k + 1
                continue
        i += 1
    return lines, funcs, order


def get(lines, funcs, name, strip_static=True):
    s, e = funcs[name]
    body = lines[s : e + 1]
    # include preceding comment lines directly attached
    return "\n".join(body) + "\n"


if __name__ == "__main__":
    lines, funcs, order = parse(sys.argv[1])
    for name in order:
        s, e = funcs[name]
        print("%5d %5d %s" % (s + 1, e + 1, name))
