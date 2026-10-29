"""Reads texture-arrival.csv (FSTextureArrivalTrace) and answers the five questions of PLAN-aurora-ideas.md section 6, step one.

usage: python -I arrival-read.py <csv> [--window SECONDS] [--tail N]   (the window runs from the 'world' row; default 90)

Rows: ms,event,id,a,b,bytes,extra. Events: wanted(a=discard then, b=boost, extra=virtual size), request(a=desired, b=discard then, extra=priority),
fetch worker states by name (a=desired, b=loaded, bytes: WAIT_HTTP_REQ requested bytes, DECODE_IMAGE formatted bytes; extra=decoded discard),
dechit(a=discard, b=format, bytes), created(a=discard, b=ok, bytes, extra=virtual size), createbreak(a=queue depth, extra=ms spent), world.

A texture's life is a series of fetch cycles (INIT ... DONE), each followed by a created row; the fast cache's thumbnail is a created row with no cycle.
Per cycle: request -> DECODE_IMAGE (the bytes are in hand: texture cache or network), DECODE_IMAGE -> DONE (decoded, or a decoded-cache hit),
DONE -> created (the create loop's budget). The dispatch queue is wanted -> first request, counted from the world if wanted earlier.
"""
import csv, sys, statistics, collections, math

path = sys.argv[1]
window = 90.0
tail_n = 15
if "--window" in sys.argv: window = float(sys.argv[sys.argv.index("--window") + 1])
if "--tail" in sys.argv: tail_n = int(sys.argv[sys.argv.index("--tail") + 1])

rows = []
with open(path, newline="") as f:
    r = csv.reader(f)
    next(r)
    for ms, ev, id_, a, b, by, ex in r:
        rows.append((float(ms) / 1000.0, ev, id_, int(a), int(b), int(by), float(ex)))

world = next((t for t, ev, *_ in rows if ev == "world"), None)
if world is None:
    print("no world row"); sys.exit(1)
rows = [(t - world, ev, id_, a, b, by, ex) for t, ev, id_, a, b, by, ex in rows if t <= world + window]
print(f"rows {len(rows)} up to {window:.0f} s after the world appeared (t=0 is the world; rows before it are kept)")

by_id = collections.defaultdict(list)
breaks = []
for t, ev, id_, a, b, by, ex in rows:
    if ev == "createbreak": breaks.append((t, a, ex))
    elif ev != "world": by_id[id_].append((t, ev, a, b, by, ex))

def pct(v, p):
    if not v: return float("nan")
    v = sorted(v); k = (len(v) - 1) * p
    lo, hi = math.floor(k), math.ceil(k)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)

def show(name, v, unit="s"):
    if not v: print(f"  {name:40s} none"); return
    print(f"  {name:40s} n {len(v):5d}  median {statistics.median(v):7.2f} {unit}  p90 {pct(v, 0.9):7.2f}  p99 {pct(v, 0.99):7.2f}  max {max(v):7.2f}")

dispatch, fetch, decode, create, whole = [], [], [], [], []
http_per_texture, bytes_http, final_bytes = [], [], []
cycles_per_texture = []
dechits = decodes = cache_loads = net_loads = 0
order = []          # (virtual size at wanted, t of the first fetched created)
never = []          # (id, last state, t of last row)
late = []           # (t created, id, timeline) for the tail
for id_, evs in by_id.items():
    evs.sort(key=lambda x: x[0])
    t_w = next((t for t, ev, *_ in evs if ev == "wanted"), None)
    vs = next((ex for t, ev, a, b, by, ex in evs if ev == "wanted"), 0.0)
    reqs = [t for t, ev, *_ in evs if ev == "request"]
    if t_w is not None and reqs:
        dispatch.append(reqs[0] - max(t_w, 0.0))
    # cycles: each INIT opens one; DECODE_IMAGE, DONE inside; the next created after DONE closes it
    cycles = []
    cur = None
    for t, ev, a, b, by, ex in evs:
        if ev == "INIT":
            cur = {"init": t, "req": None, "bytes": None, "done": None, "created": None, "http": 0, "http_bytes": 0, "dechit": False, "fmt": 0}
            cycles.append(cur)
        elif cur is None:
            continue
        elif ev == "WAIT_HTTP_REQ": cur["http"] += 1; cur["http_bytes"] += by
        elif ev == "DECODE_IMAGE" and cur["bytes"] is None: cur["bytes"] = t; cur["fmt"] = by
        elif ev == "dechit": cur["dechit"] = True
        elif ev == "DONE" and cur["done"] is None: cur["done"] = t
        elif ev == "created" and cur["done"] is not None and cur["created"] is None: cur["created"] = t
    # the request row that opened each cycle: the last 'request' at or before its INIT
    for c in cycles:
        rq = [t for t in reqs if t <= c["init"] + 0.001]
        c["req"] = rq[-1] if rq else c["init"]
        if c["bytes"] is not None: fetch.append(c["bytes"] - c["req"])
        if c["bytes"] is not None and c["done"] is not None: decode.append(c["done"] - c["bytes"])
        if c["done"] is not None and c["created"] is not None: create.append(c["created"] - c["done"])
        if c["dechit"]: dechits += 1
        elif c["done"] is not None: decodes += 1
        if c["http"]: net_loads += 1
        elif c["bytes"] is not None: cache_loads += 1
    cycles_per_texture.append(len(cycles))
    http_per_texture.append(sum(c["http"] for c in cycles))
    bytes_http.append(sum(c["http_bytes"] for c in cycles))
    final_bytes.append(max([c["fmt"] for c in cycles] or [0]))
    first_created = next((c["created"] for c in cycles if c["created"] is not None), None)
    if first_created is not None:
        if t_w is not None: whole.append(first_created - max(t_w, 0.0))
        order.append((vs, first_created))
        last_created = max(c["created"] for c in cycles if c["created"] is not None)
        late.append((last_created, id_, evs))
    elif cycles:
        last = evs[-1]
        never.append((id_, last[1], last[0], cycles[-1]["http"]))

print(f"\ntextures with a fetch {sum(1 for n in cycles_per_texture if n)}, of which never created in the window {len(never)}; fetch cycles {sum(cycles_per_texture)}")
print("\n1. where the time goes, per fetch cycle (the dispatch per texture, from the world or from wanted, whichever is later)")
show("wanted -> first request (dispatch)", dispatch)
show("request -> bytes in hand (fetch)", fetch)
show("bytes -> decoded or disk hit (decode)", decode)
show("decoded -> created (create budget)", create)
show("wanted -> first created (the whole way)", whole)

print("\n2. the ladder")
show("fetch cycles per texture", [n for n in cycles_per_texture if n], "")
show("HTTP requests per texture", http_per_texture, "")
print(f"  textures with 0 / 1 / 2 / 3+ HTTP requests: {sum(1 for n in http_per_texture if n == 0)} / {sum(1 for n in http_per_texture if n == 1)} / {sum(1 for n in http_per_texture if n == 2)} / {sum(1 for n in http_per_texture if n >= 3)}")
print(f"  finest formatted bytes held {sum(final_bytes) / 1e6:.1f} MB (the HTTP request rows carry the open range end, not a size)")
print(f"  cycles served by the texture cache {cache_loads}, by the network {net_loads}; decoded-cache hits {dechits}, decodes {decodes}")

print("\n3. the create loop")
if breaks:
    print(f"  budget breaks {len(breaks)} ({sum(1 for b in breaks if b[0] <= 30)} in the first 30 s); queue depth at the break: median {statistics.median([a for t, a, ex in breaks]):.0f}, max {max(a for t, a, ex in breaks)}; ms spent at the break: median {statistics.median([ex for t, a, ex in breaks]):.1f}")
else:
    print("  no budget breaks")

print("\n4. the order: big things first? (virtual size when wanted against when first created)")
if order:
    order.sort(key=lambda x: -x[0])
    q = max(1, len(order) // 4)
    for name, part in (("largest quarter", order[:q]), ("second", order[q:2 * q]), ("third", order[2 * q:3 * q]), ("smallest quarter", order[3 * q:])):
        if part:
            print(f"  {name:18s} n {len(part):5d}  median size {math.sqrt(statistics.median([p[0] for p in part])):6.0f} px   first created: median t={statistics.median([p[1] for p in part]):6.2f} s  p90 {pct([p[1] for p in part], 0.9):6.2f}")

if order:
    ts = sorted(p[1] for p in order)
    print("\n5. arrival curve (textures first created by time)")
    for q in (0.5, 0.8, 0.9, 0.95, 0.99, 1.0):
        print(f"  {q * 100:3.0f} % by t = {pct(ts, q):6.2f} s")

print(f"\n6. the tail: the {tail_n} latest last-created textures")
late.sort(key=lambda x: -x[0])
for t_c, id_, evs in late[:tail_n]:
    tl = " ".join(f"{ev}@{t:.1f}" + (f"(d{a}->{b},{by // 1024}k)" if ev in ("WAIT_HTTP_REQ", "DECODE_IMAGE") else (f"(d{a})" if ev in ("created", "request", "wanted") else "")) for t, ev, a, b, by, ex in evs if ev not in ("WRITE_TO_CACHE", "WAIT_ON_WRITE", "CACHE_POST", "LOAD_FROM_TEXTURE_CACHE", "DECODE_IMAGE_UPDATE", "WAIT_HTTP_RESOURCE", "SEND_HTTP_REQ", "wake"))
    print(f"  t={t_c:6.1f} {id_[:8]}: {tl[:300]}")
if never:
    print(f"\n7. never created in the window ({len(never)}): last state counts")
    for st, n in collections.Counter(x[1] for x in never).most_common(8):
        print(f"  {st:28s} {n}")
    print("  examples:")
    for id_, st, t, http in never[:8]:
        print(f"    {id_[:8]} last {st} at t={t:.1f}, http requests in its last cycle {http}")
