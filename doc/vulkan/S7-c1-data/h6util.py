import re,sys,datetime as dt
def sec(t): return int(t[0:2])*3600+int(t[3:5])*60+int(t[6:8])
for n in sys.argv[1:]:
    try:
        marks=[(l.split(' at ')[0].replace('phase ',''),re.search(r'at (\d\d:\d\d:\d\d)Z',l).group(1)) for l in open(f'<work>/run-logs/{n}.out',encoding='utf-8-sig') if l.startswith('phase') and ' at ' in l]
    except Exception as e:
        print(n,'no out',e); continue
    turn=[m for m in marks if m[0].startswith('turn')]
    if not turn: print(n,'no turn mark'); continue
    t0=sec(turn[0][1])
    ser={}
    for l in open(f'<work>/run-logs/{n}.smi.csv',errors='replace'):
        p=[x.strip() for x in l.split(',')]
        if len(p)<4: continue
        m=re.match(r'\d+/\d+/\d+ (\d\d:\d\d:\d\d)',p[0])
        if not m: continue
        s=(sec(m.group(1))+5*3600)%86400   # CDT -> UTC
        try: ser[s-t0]=float(p[1].split()[0])
        except: pass
    row=[]
    for a in range(-10,75,5):
        v=[ser[k] for k in range(a,a+5) if k in ser]
        row.append('%3d'%(sum(v)/len(v)) if v else '  -')
    print('%-10s'%n,' '.join(row))
print('t(s) rel. turn start: -10 -5 0 5 ... 70')
