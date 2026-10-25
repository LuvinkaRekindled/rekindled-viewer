import re, sys
def sec(t): return int(t[0:2])*3600+int(t[3:5])*60+int(t[6:8])
def load(n):
    marks={}
    for l in open(f'<work>/run-logs/{n}.out',encoding='utf-8-sig'):
        m=re.match(r'phase (still1|turn|back|still2|end)',l); t=re.search(r'at (\d\d:\d\d:\d\d)Z',l)
        if m and t: marks[m.group(1)]=sec(t.group(1))
    return marks, open(f'<work>/run-logs/{n}.log',errors='replace').read().split('\n')
def phase_of(x,m):
    if m['still1']<=x<m['turn']: return 'still1'
    if m['turn']<=x<m['back']: return 'turn'
    if m['back']<=x<m['end']: return 'after'
    return None
for n in sys.argv[1:]:
    m,lines=load(n)
    ch={}; desc={'still1':[0,0,0,0,0],'turn':[0,0,0,0,0],'after':[0,0,0,0,0]}
    pts=[]
    zon={'still1':[],'turn':[],'after':[]}
    rec=[]
    for l in lines:
        t=re.search(r'T(\d\d:\d\d:\d\d)Z',l)
        if not t: continue
        x=sec(t.group(1))
        if 'VkTexCopy' in l:
            g=re.search(r'mip chains grown (\d+)',l); d=re.search(r'levels dropped (\d+)',l); c=re.search(r'images committed under the same id \(a new resolution\) (\d+)',l)
            if g and d and c: pts.append((x,int(g.group(1)),int(d.group(1)),int(c.group(1))))
        if 'idxLog' in l and 'desired level' in l:
            ph=phase_of(x-5,m)  # the line covers the 10 s before it
            mm=re.search(r'desired level: visits (\d+), changed (\d+) \(finer (\d+), coarser (\d+), (\d+) levels\)',l)
            if ph and mm:
                for i in range(5): desc[ph][i]+=int(mm.group(i+1))
        if '#CpuZones#' in l:
            ph=phase_of(x-5,m)
            body=l.split('inclusive zones, nested ones count in their parents) | ',1)
            if ph and len(body)>1:
                d={}
                for part in body[1].split(' | '):
                    mm=re.match(r'(.+?) ([\d.]+) ms',part.strip())
                    if mm: d[mm.group(1)]=float(mm.group(2))
                zon[ph].append(d)
        if 'idxLog' in l and 'face records' in l:
            f=re.search(r'frame\(\) ([\d.]+) ms a frame',l)
            if f: rec.append(float(f.group(1)))
    def at(t):
        b=pts[0]
        for p in pts:
            if p[0]<=t: b=p
        return b
    a=[at(m['still1']),at(m['turn']),at(m['back']),at(m['end'])]
    def zavg(ph,k):
        xs=[d[k] for d in zon[ph] if k in d]; return sum(xs)/len(xs) if xs else float('nan')
    print(n)
    for i,ph in enumerate(['still1','turn','after']):
        dg,dd,dc=a[i+1][1]-a[i][1],a[i+1][2]-a[i][2],a[i+1][3]-a[i][3]
        s=desc[ph]
        print('  %-6s grown %6d dropped %6d commits %6d | desired level: visits %7d changed %6d (%.2f%%) finer %5d coarser %5d | list %.2f face loop %.2f interval %.1f'%(ph,dg,dd,dc,s[0],s[1],100.0*s[1]/max(s[0],1),s[2],s[3],zavg(ph,'display: gTextureList.updateImages'),zavg(ph,'textures: decode priority: face loop'),zavg(ph,'loop interval') if False else float('nan')))
    if rec: print('  frame() upkeep ms/frame (mean of 10 s lines): %.3f'%(sum(rec)/len(rec)))
