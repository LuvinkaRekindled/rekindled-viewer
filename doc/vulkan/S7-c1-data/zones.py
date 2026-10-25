import re, sys
# per-phase average (ms per frame) of the texture zones of the main thread, from the "CpuZones" log lines (one per 600 frames); phases from the lap's .out
NAMES = ['display: gTextureList.updateImages', 'textures: fetch dispatch', 'textures: decode priority (per texture)', 'textures: decode priority: face loop',
         'textures: updateFetch (per texture)', 'textures: create from decoded', 'main loop: display()', 'main loop: idle()']
def sec(t): return int(t[0:2]) * 3600 + int(t[3:5]) * 60 + int(t[6:8])
for n in sys.argv[1:]:
    marks = {}
    for l in open(f'<work>/run-logs/{n}.out', encoding='utf-8-sig'):
        m = re.match(r'phase (still1|turn|back|still2|end)', l)
        t = re.search(r'at (\d\d:\d\d:\d\d)Z', l)
        if m and t: marks[m.group(1)] = sec(t.group(1))
    phases = [('still1', marks['still1'], marks['turn']), ('turn', marks['turn'], marks.get('back', marks['turn'] + 60)), ('still2', marks.get('still2', 10**9), marks.get('end', 10**9 + 1))]
    rows = []
    for l in open(f'<work>/run-logs/{n}.log', errors='replace'):
        if '#CpuZones#' not in l: continue
        t = re.search(r'T(\d\d:\d\d:\d\d)Z', l)
        if not t: continue
        vals = {}
        for nm in NAMES:
            m = re.search(re.escape(nm) + r' ([\d.]+) ms', l)
            if m: vals[nm] = float(m.group(1))
        m = re.search(r'loop interval ([\d.]+) ms', l)
        if m: vals['loop interval'] = float(m.group(1))
        rows.append((sec(t.group(1)), vals))
    print(n)
    for ph, a, b in phases:
        sel = [v for (t, v) in rows if a + 20 <= t < b]
        if not sel: print('  ', ph, 'no lines'); continue
        out = []
        for nm in ['loop interval'] + NAMES:
            xs = [v[nm] for v in sel if nm in v]
            if xs: out.append('%s %.3f' % (nm.split(': ', 1)[-1][:28], sum(xs) / len(xs)))
        print('  ', ph, '(%d lines)' % len(sel), ' | '.join(out))
