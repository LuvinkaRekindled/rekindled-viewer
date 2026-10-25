import re, sys
def sec(t): return int(t[0:2])*3600+int(t[3:5])*60+int(t[6:8])
def load(n):
    marks={}
    for l in open(f'<work>/run-logs/{n}.out',encoding='utf-8-sig'):
        m=re.match(r'phase (still1|turn|back|still2|end)',l); t=re.search(r'at (\d\d:\d\d:\d\d)Z',l)
        if m and t: marks[m.group(1)]=sec(t.group(1))
    rows=[]
    for l in open(f'<work>/run-logs/{n}.log',errors='replace'):
        if '#CpuZones#' not in l: continue
        t=re.search(r'T(\d\d:\d\d:\d\d)Z',l)
        if not t: continue
        body=l.split('parents) | ',1)
        if len(body)<2: continue
        z={}
        m=re.search(r'loop interval ([\d.]+) ms',l)
        if m: z['(loop interval)']=float(m.group(1))
        for part in body[1].split(' | '):
            m=re.match(r'(.*?) ([\d.]+) ms \(([\d.]+) calls\)',part.strip())
            if m: z[m.group(1)]=(float(m.group(2)),float(m.group(3)))
        rows.append((sec(t.group(1)),z))
    return marks,rows
def avg(rows,a,b):
    sel=[z for t,z in rows if a<=t<b]
    out={}
    for z in sel:
        for k,v in z.items():
            val=v if isinstance(v,float) else v[0]
            out.setdefault(k,[]).append(val)
    return {k:sum(v)/len(v) for k,v in out.items()},len(sel)
for n in sys.argv[1:]:
    marks,rows=load(n)
    a1,n1=avg(rows,marks['still1']+15,marks['turn'])
    a2,n2=avg(rows,marks['still2']+15,marks.get('end',10**9))
    print(n,'windows',n1,n2)
    diffs=[(a2.get(k,0)-a1.get(k,0),k,a1.get(k,0),a2.get(k,0)) for k in set(a1)|set(a2)]
    diffs.sort(reverse=True)
    for d,k,x,y in diffs[:14]:
        print('  %+.3f ms  %-55s %.3f -> %.3f'%(d,k[:55],x,y))
    print('  ...biggest decreases:')
    for d,k,x,y in diffs[-3:]:
        print('  %+.3f ms  %-55s %.3f -> %.3f'%(d,k[:55],x,y))
