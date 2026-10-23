import re,sys,collections
fn=sys.argv[1]; skip=int(sys.argv[2]) if len(sys.argv)>2 else 2
num=re.compile(r'-?\d+\.?\d*')
d=collections.OrderedDict(); cnt=collections.Counter()
for l in open(fn,errors='replace'):
    if '#VisCensus#' not in l: continue
    t=l.split('report : ',1)[1].strip()
    nums=[float(x) for x in num.findall(t)]
    # key: text with numbers masked, but keep the pass/partition/B0 name (letters-only prefix up to first ':')
    key=num.sub('#',t)
    d.setdefault(key,[]).append((nums,t))
out=[]
for k,v in d.items():
    v=v[skip:] if len(v)>skip+1 else v
    n=len(v); m=[sum(x[0][i] for x in v)/n for i in range(len(v[0][0]))]
    it=iter(m)
    s=num.sub(lambda mo:('%.3f'%next(it)).rstrip('0').rstrip('.'),v[0][1])
    out.append('[%d win] %s'%(n,s))
print('\n'.join(out))
