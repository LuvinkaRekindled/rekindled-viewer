import re,sys,collections
fn=sys.argv[1]; skip=3
rows=[]
for l in open(fn,errors='replace'):
    if '#CpuZones#' in l and 'main thread per frame' in l:
        d={}
        for m in re.finditer(r'\| ([^|]+?) ([\d.]+) ms \(([\d.]+) calls\)',l):
            d[m.group(1)]=(float(m.group(2)),float(m.group(3)))
        m=re.search(r'loop interval ([\d.]+) ms',l); d['LOOP']=(float(m.group(1)),1)
        rows.append(d)
rows=rows[skip:]
print(len(rows),'windows')
names=collections.OrderedDict()
for r in rows:
    for k in r: names[k]=1
want=sys.argv[2:]
for k in names:
    if want and not any(w in k for w in want): continue
    v=[r[k] for r in rows if k in r]
    print('%-62s %6.3f ms  %6.1f calls  (%d/%d windows)'%(k,sum(x[0] for x in v)/len(rows),sum(x[1] for x in v)/len(v),len(v),len(rows)))
