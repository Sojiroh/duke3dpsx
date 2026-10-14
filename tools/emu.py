#!/usr/bin/env python3
"""Headless PCSX-Redux test harness.

Usage: emu.py [--iso build/duke3d.cue | --exe build/duke3d.exe] [--out DIR] [--timeout S]
              [--memcard FILE] [--watchdog N] [--dynarec] ACTION ...
Actions (frame-scheduled, executed in order; a frame = one vsync):
  wait N            wait N frames
  press BTN[+BTN] N hold buttons for N frames (then release). BTN: CROSS CIRCLE SQUARE TRIANGLE
                    UP DOWN LEFT RIGHT L1 L2 R1 R2 START SELECT
  hold BTN[+BTN]    press and keep held;   release BTN[+BTN]
  shot NAME         save screenshot NAME.png in --out
  vram NAME         dump the whole VRAM as raw 16-bit pixels to NAME.vram in --out (no PNG)
  pc N              print the CPU state (EXCEPTION ... (sample) lines, symbolized) every frame for N frames
  prof N            sample the PC every frame for N frames and print a PROFILE histogram (by function and by caller)
  quit              print the final CPU state + FPS and exit emulator
Prints the game's TTY output.  Symbols (watchdog address, symbolizer, profile) come from the ELF next to
--exe (duke3d.elf), or from $BUILD_DIR/duke3d.elf when only --iso is used.
--watchdog N: the game's video_framecount is watched; after 240 frames without a new game frame a WATCHDOG
line is printed and repeated every 40 frames; N such lines in a row (the count restarts whenever the game
advances a frame) end the run with exit code 4 and hang.png.  A level load takes ~2000 frames, hence the
default of 60.
Exit code: 0 when the script completed (also at the end of the actions or after `quit`; no crash.png), 3 on a
CPU exception (crash.png + EXCEPTION line), 4 watchdog, 124 --timeout.
"""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REDUX = os.path.join(ROOT, "tools/redux/PCSX-Redux-HEAD-x86_64.AppImage")
ELF = os.path.join(ROOT, os.environ.get("BUILD_DIR", "build"), "duke3d.elf")  # set from --exe in main()


def symbol_addr(name):
    elf = ELF
    nm = os.path.join(ROOT, "tools/gcc/bin/mipsel-none-elf-nm")
    for l in subprocess.run([nm, elf], capture_output=True, text=True).stdout.splitlines():
        parts = l.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16) & 0xffffffff
    return 0


def gen_lua(actions, outdir, watch_addr=0, watchdog=12):
    lines = ["local acts = {"]
    for a in actions:
        w = a.split()
        if w[0] == "wait":
            lines.append("  {op='wait', n=%d}," % int(w[1]))
        elif w[0] in ("press", "hold", "release"):
            btns = ",".join("PCSX.CONSTS.PAD.BUTTON.%s" % b for b in w[1].split("+"))
            n = max(1, int(w[2])) if w[0] == "press" else 0
            lines.append("  {op='%s', b={%s}, n=%d}," % (w[0], btns, n))
        elif w[0] == "shot":
            lines.append("  {op='shot', name='%s'}," % w[1])
        elif w[0] == "vram":
            lines.append("  {op='vram', name='%s'}," % w[1])
        elif w[0] == "quit":
            lines.append("  {op='quit'},")
        elif w[0] == "pc":
            lines.append("  {op='pc', n=%d}," % int(w[1]))
        elif w[0] == "prof":
            lines.append("  {op='prof', n=%d}," % int(w[1]))
        else:
            raise SystemExit("bad action: " + a)
    lines.append("}")
    lines.append(r"""
local outdir = '%s'
local watch_addr = %d
local watchdog_limit = %d

local pad = PCSX.SIO0.slots[1].pads[1]
local mem = PCSX.getMemPtr()
local lastfc, stall, samples = -1, 0, 0
local function framecount()
  if watch_addr == 0 then return 0 end
  local a = bit.band(watch_addr, 0x1fffff)
  return mem[a] + mem[a+1]*256 + mem[a+2]*65536 + mem[a+3]*16777216
end
local idx, left, holding = 1, 0, nil
local frames = 0
local function shot(name)
  local ss = PCSX.GPU.takeScreenShot()
  local f = Support.File.open(outdir .. '/' .. name .. '.raw', 'TRUNCATE')
  f:writeMoveSlice(ss.data); f:close()
  local m = Support.File.open(outdir .. '/' .. name .. '.meta', 'TRUNCATE')
  m:write(string.format('%%d %%d\n', ss.width, ss.height)); m:close()
  print('SHOT ' .. name .. ' frame ' .. frames)
end
local function step()
  while true do
    local a = acts[idx]
    if a == nil then quitting = true; PCSX.quit(0); return end
    if left > 0 then left = left - 1; if left == 0 and holding then for _,b in ipairs(holding) do pad.clearOverride(b) end; holding = nil; idx = idx + 1 end; return end
    if a.op == 'wait' then left = a.n; idx = idx + 1; return
    elseif a.op == 'press' then for _,b in ipairs(a.b) do pad.setOverride(b) end; holding = a.b; left = a.n; return
    elseif a.op == 'hold' then for _,b in ipairs(a.b) do pad.setOverride(b) end; idx = idx + 1
    elseif a.op == 'release' then for _,b in ipairs(a.b) do pad.clearOverride(b) end; idx = idx + 1
    elseif a.op == 'shot' then shot(a.name); idx = idx + 1
    elseif a.op == 'vram' then
      local v = PCSX.GPU.getVRAM()
      local f = Support.File.open(outdir .. '/' .. a.name .. '.vram', 'TRUNCATE')
      f:writeMoveSlice(v); f:close()
      print('VRAM ' .. a.name .. ' frame ' .. frames)
      idx = idx + 1
    elseif a.op == 'prof' then
      local r = PCSX.getRegisters()
      print(string.format('PCSAMPLE %%08x %%08x', r.pc, r.GPR.n.ra))
      a.n = a.n - 1; if a.n <= 0 then idx = idx + 1 end; return
    elseif a.op == 'pc' then
      local r = PCSX.getRegisters()
      print(string.format('EXCEPTION pc=%%08x ra=%%08x sp=%%08x (sample)', r.pc, r.GPR.n.ra, r.GPR.n.sp))
      a.n = a.n - 1; if a.n <= 0 then idx = idx + 1 end; return
    elseif a.op == 'quit' then
      local r = PCSX.getRegisters()
      print(string.format('EXCEPTION pc=%%08x ra=%%08x sp=%%08x (final state, not a crash)', r.pc, r.GPR.n.ra, r.GPR.n.sp))
      print('FPS ' .. tostring(PCSX.GPU.getGuestFPS())); quitting = true; PCSX.quit(0); return end
  end
end
function DrawImguiFrame() end
-- the returned listener objects have a __gc that removes the listener: keep them referenced for the whole run
-- (unreferenced, LuaJIT's GC eventually collects them and the script silently stops getting vsync events)
keep_vsync = PCSX.Events.createEventListener('GPU::Vsync', function()
  frames = frames + 1
  local fc = framecount()
  if fc ~= lastfc then lastfc = fc; stall = 0; samples = 0 else stall = stall + 1 end  -- recovered stalls do not accumulate
  if lastfc > 0 and stall > 240 then
    local r = PCSX.getRegisters()
    print(string.format('EXCEPTION pc=%%08x ra=%%08x sp=%%08x (WATCHDOG: no frame for 240 vsyncs, frame %%d)', r.pc, r.GPR.n.ra, r.GPR.n.sp, lastfc))
    samples = samples + 1
    if samples >= watchdog_limit then shot('hang'); quitting = true; PCSX.quit(4) end
    stall = 200
    return
  end
  step()
end)
keep_pause = PCSX.Events.createEventListener('ExecutionFlow::Pause', function(ev)
  if quitting then return end
  local r = PCSX.getRegisters()
  print(string.format('EXCEPTION pc=%%08x ra=%%08x sp=%%08x epc=%%08x cause=%%08x badv=%%08x a0=%%08x a1=%%08x (PAUSE exception=%%s)', r.pc, r.GPR.n.ra, r.GPR.n.sp, r.CP0.r[14], r.CP0.r[13], r.CP0.r[8], r.GPR.n.a0, r.GPR.n.a1, tostring(ev.exception)))
  shot('crash')
  PCSX.quit(3)
end)
""" % (outdir, watch_addr, watchdog))
    return "\n".join(lines)


def symbolize(ln):
    if not ln.startswith("EXCEPTION"):
        return ln
    import bisect, re
    elf = ELF
    nm = os.path.join(ROOT, "tools/gcc/bin/mipsel-none-elf-nm")
    syms = []
    for l in subprocess.run([nm, "-n", elf], capture_output=True, text=True).stdout.splitlines():
        parts = l.split()
        if len(parts) == 3 and parts[1] in "tT":
            syms.append((int(parts[0], 16) & 0xffffffff, parts[2]))
    addrs = [a for a, _ in syms]

    def sym(v):
        i = bisect.bisect_right(addrs, v) - 1
        return "%s+0x%x" % (syms[i][1], v - syms[i][0]) if i >= 0 else "?"
    return re.sub(r"(pc|ra|epc)=([0-9a-f]{8})", lambda m: "%s=%s(%s)" % (m.group(1), m.group(2), sym(int(m.group(2), 16))), ln)


def report_profile(samples):
    import bisect
    elf = ELF
    nm = os.path.join(ROOT, "tools/gcc/bin/mipsel-none-elf-nm")
    syms = []
    for l in subprocess.run([nm, "-n", elf], capture_output=True, text=True).stdout.splitlines():
        parts = l.split()
        if len(parts) == 3 and parts[1] in "tT":
            syms.append((int(parts[0], 16) & 0xffffffff, parts[2]))
    addrs = [a for a, _ in syms]

    def sym(v):
        i = bisect.bisect_right(addrs, v) - 1
        return syms[i][1] if i >= 0 else "?"
    hist, hist2 = {}, {}
    for ln in samples:
        _, pc, ra = ln.split()
        s = sym(int(pc, 16))
        hist[s] = hist.get(s, 0) + 1
        k = s + " <- " + sym(int(ra, 16))
        hist2[k] = hist2.get(k, 0) + 1
    n = len(samples)
    print("PROFILE %d samples" % n)
    for s, c in sorted(hist.items(), key=lambda kv: -kv[1])[:22]:
        print("  %5.1f%% %s" % (100.0 * c / n, s))
    print("  --- by caller ---")
    for s, c in sorted(hist2.items(), key=lambda kv: -kv[1])[:16]:
        print("  %5.1f%% %s" % (100.0 * c / n, s))


def raw_to_png(outdir, name):
    meta = os.path.join(outdir, name + ".meta")
    if not os.path.exists(meta):
        return False
    w, h = map(int, open(meta).read().split())
    if w == 0 or h == 0:  # display not enabled yet: nothing to convert
        os.remove(meta); os.remove(os.path.join(outdir, name + ".raw"))
        print("SHOT %s: display off, skipped" % name)
        return False
    raw = np.fromfile(os.path.join(outdir, name + ".raw"), dtype=np.uint8)
    if len(raw) == w * h * 2:
        px = raw.view(np.uint16).reshape(h, w)
        r = (px & 31) << 3; g = ((px >> 5) & 31) << 3; b = ((px >> 10) & 31) << 3
        img = np.stack([r, g, b], axis=-1).astype(np.uint8)
    else:
        img = raw[: w * h * 3].reshape(h, w, 3)
    Image.fromarray(img).save(os.path.join(outdir, name + ".png"))
    os.remove(meta); os.remove(os.path.join(outdir, name + ".raw"))
    return True


def main():
    ap = argparse.ArgumentParser()
    bd = os.environ.get("BUILD_DIR", "build")
    global ELF
    ap.add_argument("--iso", default=os.path.join(ROOT, bd, "duke3d.cue"))
    ap.add_argument("--exe")
    ap.add_argument("--out", default=os.path.join(ROOT, bd, "shots"))
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--memcard")
    ap.add_argument("--dynarec", action="store_true")
    ap.add_argument("--watchdog", type=int, default=60,
                    help="quit (exit 4, hang.png) after N stall samples; the first comes after 240 vsyncs without a game frame, then one per 40 vsyncs")
    ap.add_argument("actions", nargs="*")
    args = ap.parse_args()
    # the ELF (watchdog symbol, symbolizer, profile) must belong to the executable being run
    ELF = os.path.splitext(os.path.abspath(args.exe))[0] + ".elf" if args.exe else os.path.join(ROOT, bd, "duke3d.elf")
    if not os.path.exists(ELF):
        print("warning: %s not found: no watchdog/symbols" % ELF, file=sys.stderr)
    os.makedirs(args.out, exist_ok=True)
    actions = args.actions or ["wait 300", "shot boot", "quit"]
    lua = os.path.join(args.out, "script.lua")
    open(lua, "w").write(gen_lua(actions, args.out, symbol_addr("video_framecount"), args.watchdog))
    cmd = [REDUX, "-no-ui", "-run", "-fastboot", "-no-update", "-stdout", "-lua_stdout",
           "-dofile", lua, "-interpreter" if not args.dynarec else "-dynarec", "-2mb"]
    cmd += ["-iso", args.iso]
    if args.exe:
        cmd += ["-exe", args.exe]
    if args.memcard:
        cmd += ["-memcard1", args.memcard]
    env = dict(os.environ, HOME=os.path.join(args.out, "home"))
    os.makedirs(env["HOME"], exist_ok=True)
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout, env=env)
        out = p.stdout + p.stderr
        rc = p.returncode
    except subprocess.TimeoutExpired as ex:
        out = (ex.stdout or b"").decode(errors="replace") + "\nTIMEOUT"
        rc = 124
    keep = []
    for ln in out.splitlines():
        if not ln.strip():
            continue
        if ln.startswith("(String)") and (".lua" in ln or "attempt" in ln or "error" in ln.lower()):
            keep.append("LUA: " + ln)
            continue
        if ln.startswith(("(Table)", "(String)")) or "ImGui" in ln:
            continue
        keep.append(ln)
    samples = [ln for ln in keep if ln.startswith("PCSAMPLE")]
    keep = [symbolize(ln) for ln in keep if not ln.startswith("PCSAMPLE")]
    print("\n".join(keep[-400:]))
    if samples:
        report_profile(samples)
    for a in actions:
        if a.startswith("shot "):
            raw_to_png(args.out, a.split()[1])
    raw_to_png(args.out, "crash")
    raw_to_png(args.out, "hang")
    sys.exit(0 if rc == 0 else rc)


if __name__ == "__main__":
    main()
