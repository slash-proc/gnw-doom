#!/usr/bin/env python3
"""Pull the gnw-doom pipeline trace ring over SWD and write a detailed timing
report (TRACE=1 builds; see src/gnw/trace_gnw.h).

Usage: tracepull.py [out.log] [binary.out]

Report contents:
  - per-frame breakdown: every stage's duration in us + each decode event
  - per-stage totals/averages across the capture window
  - CPU accounting per frame: busy = frame - idle
"""
import struct, subprocess, sys, collections
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

CPU_HZ = 280_000_000
BIN = sys.argv[2] if len(sys.argv) > 2 else "build/shareware/doom.out"

EV = ["NONE","FRAME","TICS_BEG","TICS_END","GTIC_BEG","GTIC_END",
      "RENDER_BEG","RENDER_END","BSP_BEG","BSP_END","FLATS_BEG","FLATS_END",
      "FLATDEC_BEG","FLATDEC_END","PATCHDEC_BEG","PATCHDEC_END",
      "REGCOLS_BEG","REGCOLS_END","FUZZ_BEG","FUZZ_END",
      "OVERLAY_BEG","OVERLAY_END","COMPOSE_BEG","COMPOSE_END",
      "MIX_BEG","MIX_END","OPL_BEG","OPL_END","IDLE_BEG","IDLE_END","WIPE",
      "CMP_BASE","CMP_OVERLAY","CMP_OUT","LOAD_BEG","LOAD_END"]

def sym(name):
    for l in subprocess.check_output(["arm-none-eabi-nm", BIN]).decode().splitlines():
        p = l.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    raise SystemExit(f"symbol {name} not found — TRACE=1 build flashed?")

buf_addr = sym("trace_buf")
head_addr = sym("trace_head")
ENTRIES = 32768  # keep in sync with TRACE_ENTRIES (trace_gnw.h)

b = OpenOCDBackend(); b.open()
b.halt()
head = struct.unpack("<I", b.read_memory(head_addr, 4))[0]
raw = b.read_memory(buf_addr, ENTRIES * 8)
b.resume(); b.close()

n = min(head, ENTRIES)
first = head - n
events = []
for i in range(first, head):
    off = (i % ENTRIES) * 8
    cyc, ev, arg = struct.unpack_from("<IHH", raw, off)
    events.append((cyc, ev, arg))

out = open(sys.argv[1] if len(sys.argv) > 1 else "build/trace.log", "w")
out.write(f"gnw-doom pipeline trace: {n} events (head={head}, wrapped={head > ENTRIES})\n")
out.write(f"cycle clock {CPU_HZ/1e6:.0f} MHz; all durations in us\n\n")

def us(c):
    return c / (CPU_HZ / 1_000_000)

# pair *_BEG/*_END streams, attribute to frames
stack = {}
totals = collections.Counter()
counts = collections.Counter()
maxes = collections.defaultdict(int)
frame_no = None
frame_start = None
frame_lines = []
frame_stage = collections.Counter()
frames_done = 0
per_frame_rows = []

def flush_frame(end_cyc):
    global frame_lines, frame_stage, frames_done
    if frame_no is None:
        return
    total = us((end_cyc - frame_start) & 0xFFFFFFFF)
    idle = frame_stage.get("IDLE", 0)
    busy = total - idle
    cpu = 100 * busy / total if total > 0 else 0
    per_frame_rows.append((frame_no, total, busy, cpu, dict(frame_stage), list(frame_lines)))
    frames_done += 1
    frame_lines = []
    frame_stage.clear()

for cyc, ev, arg in events:
    name = EV[ev] if ev < len(EV) else f"?{ev}"
    if name == "FRAME":
        flush_frame(cyc)
        frame_no = arg
        frame_start = cyc
        continue
    if name.endswith("_BEG"):
        stack[name[:-4]] = (cyc, arg)
    elif name.endswith("_END"):
        key = name[:-4]
        if key in stack:
            c0, a0 = stack.pop(key)
            d = us((cyc - c0) & 0xFFFFFFFF)
            totals[key] += d
            counts[key] += 1
            maxes[key] = max(maxes[key], d)
            if frame_no is not None:
                frame_stage[key] += d
                if key in ("FLATDEC", "PATCHDEC", "OPL", "MIX"):
                    frame_lines.append(f"      {key} arg={a0} {d:8.1f}us")
    elif name == "WIPE" and frame_no is not None:
        frame_lines.append(f"      WIPE wipe_min={arg}")
    elif name.startswith("CMP_"):
        # compose sub-phase, arg = microseconds for the whole frame
        totals[name] += arg
        counts[name] += 1
        maxes[name] = max(maxes[name], arg)
        if frame_no is not None:
            frame_stage[name] += arg

out.write("=== per-stage totals over capture ===\n")
out.write(f"{'stage':10} {'calls':>7} {'total us':>12} {'avg us':>9} {'max us':>9}\n")
for k, t in totals.most_common():
    out.write(f"{k:10} {counts[k]:7d} {t:12.1f} {t/counts[k]:9.1f} {maxes[k]:9.1f}\n")

out.write("\n=== per-frame breakdown ===\n")
for fn, total, busy, cpu, stages, lines in per_frame_rows:
    out.write(f"frame {fn}: total {total:8.1f}us busy {busy:8.1f}us cpu {cpu:5.1f}%\n")
    for k in ("TICS","RENDER","BSP","FLATS","REGCOLS","FUZZ","OVERLAY","COMPOSE","MIX","OPL","IDLE"):
        if k in stages:
            out.write(f"    {k:8} {stages[k]:9.1f}us\n")
    for l in lines:
        out.write(l + "\n")

out.close()
print(f"frames captured: {frames_done}; events: {n}")
print("report:", sys.argv[1] if len(sys.argv) > 1 else "build/trace.log")
# quick stdout summary
tot_all = sum(t for k, t in totals.items() if k != "IDLE")
print("\nstage totals (us, excl IDLE):")
for k, t in totals.most_common(12):
    print(f"  {k:10} {t:12.1f} ({counts[k]} calls, avg {t/counts[k]:.1f}, max {maxes[k]:.1f})")
