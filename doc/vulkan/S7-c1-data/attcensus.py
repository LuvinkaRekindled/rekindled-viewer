import re, sys
def sec(t): return int(t[0:2])*3600+int(t[3:5])*60+int(t[6:8])
for n in sys.argv[1:]:
    marks={}
    for l in open(f'<work>/run-logs/{n}.out',encoding='utf-8-sig'):
        m=re.match(r'phase (still1|turn|back|still2|end)',l); t=re.search(r'at (\d\d:\d\d:\d\d)Z',l)
        if m and t: marks[m.group(1)]=sec(t.group(1))
    def ph(x):
        if x<marks['still1']: return 'load'
        if x<marks['turn']: return 'still1'
        if x<marks.get('back',10**9): return 'turn'
        if x<marks.get('still2',10**9): return 'back'
        return 'still2'
    agg={}
    for l in open(f'<work>/run-logs/{n}.log',errors='replace'):
        if '#AttCensus#' not in l: continue
        t=re.search(r'T(\d\d:\d\d:\d\d)Z',l); 
        if not t: continue
        p=ph(sec(t.group(1)))
        body=l.split(': ',1)[1] if ': ' in l else l
        m=re.search(r'(\S.*?) per frame:(.*)$',body)
        key=None; nums=None
        if body.lstrip().startswith(('drawable updateMove','bridge updateMove','octree partition move')) or 'partition moves and the moved list' in body:
            key=re.match(r'\s*(.*?)( per frame)',body.split('LL_ENDL')[0]).group(1) if False else body.split(' per frame')[0].split('operator () : ')[-1]
            nums=[float(x) for x in re.findall(r'(\d+\.\d+)',body.split('per frame:')[1])]
        elif 'rigid attachments per frame in the loop' in body:
            key='loop'; nums=[float(x) for x in re.findall(r'(\d+\.\d+)',body)]
        if key is None: continue
        a=agg.setdefault((p,key),[0,[0.0]*len(nums)])
        a[0]+=1
        for i,v in enumerate(nums): a[1][i]+=v
    print(n)
    for (p,key),(c,s) in sorted(agg.items(), key=lambda kv:(['load','still1','turn','back','still2'].index(kv[0][0]),kv[0][1])):
        if p=='load': continue
        print('  %-6s %-45s %s'%(p,key[:45],' '.join('%.3f'%(v/c) for v in s)))
