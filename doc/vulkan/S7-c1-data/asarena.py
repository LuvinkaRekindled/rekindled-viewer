import re, sys
def sec(t): return int(t[0:2])*3600+int(t[3:5])*60+int(t[6:8])
for n in sys.argv[1:]:
    marks={}
    for l in open(f'<work>/run-logs/{n}.out',encoding='utf-8-sig'):
        m=re.match(r'phase (still1|turn|back|still2|end)',l); t=re.search(r'at (\d\d:\d\d:\d\d)Z',l)
        if m and t: marks[m.group(1)]=sec(t.group(1))
    def ph(x):
        if 'still1' not in marks or x<marks['still1']: return 'load'
        if x<marks['turn']: return 'still1'
        if 'back' not in marks or x<marks['back']: return 'turn'
        return 'after'
    agg={}
    for l in open(f'<work>/run-logs/{n}.log',errors='replace'):
        if '#VkAsArena#' not in l: continue
        t=re.search(r'T(\d\d:\d\d:\d\d)Z',l)
        m=re.search(r'arena: (\d+) blocks alive \((\d+) MB, (\d+) MB in use, largest free range (\d+) MB\), of them (\d+) dedicated \((\d+) MB\); in this interval: (\d+) ranges asked for \((\d+) MB; <1 MB (\d+), <8 (\d+), <32 (\d+), <64 (\d+), <128 (\d+), more (\d+)\), blocks made (\d+) \(dedicated (\d+), (\d+) MB, ([\d.]+) ms in vmaCreateBuffer = ([\d.]+) ms/frame\), blocks given back (\d+) \(dedicated (\d+), lived ([\d.]+) frames',l)
        if not (t and m): continue
        pm=re.search(r'reused (\d+), not found (\d+), given back after a wait (\d+), holding now (\d+) blocks \((\d+) MB\)',l)
        p=ph(sec(t.group(1))); g=[float(x) for x in m.groups()]
        a=agg.setdefault(p,{'n':0,'made':0,'dmade':0,'madeMB':0,'dback':0,'life':0,'hit':0,'miss':0,'trim':0,'pooled':0,'pooledmb':0,'alive':0,'alivemb':0})
        a['n']+=1; a['made']+=g[14]; a['dmade']+=g[15]; a['madeMB']+=g[16]; a['dback']+=g[20]; a['life']+=g[21]*g[20]; a['alive']=max(a['alive'],g[0]); a['alivemb']=max(a['alivemb'],g[1])
        if pm:
            a['hit']+=int(pm.group(1)); a['miss']+=int(pm.group(2)); a['trim']+=int(pm.group(3)); a['pooled']=max(a['pooled'],int(pm.group(4))); a['pooledmb']=max(a['pooledmb'],int(pm.group(5)))
    print(n)
    for p in ['load','still1','turn','after']:
        if p not in agg: continue
        a=agg[p]; s=10.0*a['n']
        print('  %-6s dedicated blocks made %5.1f/s (%4d MB/s) | pool: reused %5.1f/s, not found %5.1f/s, trimmed %.2f/s, most held %d blocks (%d MB) | max alive %d blocks (%d MB)'%(p,a['dmade']/s,a['madeMB']/s,a['hit']/s,a['miss']/s,a['trim']/s,a['pooled'],a['pooledmb'],a['alive'],a['alivemb']))
