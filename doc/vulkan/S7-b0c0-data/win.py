import re,sys,statistics as st
fn=sys.argv[1]
fps=[];avg=[];p50=[];p95=[];mw=[];ww=[];sp=[];pr=[];ops=[];rt=[];gpu=[]
for l in open(fn,errors='replace'):
    m=re.search(r'display_stats : FPS: ([\d.]+)',l)
    if m: fps.append(float(m.group(1)))
    m=re.search(r'frame starts on the main thread \(ms between\): n (\d+), avg ([\d.]+), p50 ([\d.]+), p95 ([\d.]+).*main thread waited for the worker ([\d.]+) ms/frame, worker waited for work ([\d.]+)',l)
    if m: avg.append(float(m.group(2)));p50.append(float(m.group(3)));p95.append(float(m.group(4)));mw.append(float(m.group(5)));ww.append(float(m.group(6)))
    m=re.search(r'([\d.]+) ms/frame in operations out of ([\d.]+) ms/frame wall',l)
    if m: ops.append(float(m.group(1)))
    m=re.search(r'\| shadowPackets \d+ calls ([\d.]+) ms',l)
    if m: sp.append(float(m.group(1)))
    m=re.search(r'\| present \d+ calls ([\d.]+) ms',l)
    if m: pr.append(float(m.group(1)))
    m=re.search(r'rtScene \d+ calls ([\d.]+) ms',l)
    if m: rt.append(float(m.group(1)))
    m=re.search(r'GPU time of 600 presented frames: up to 12 ms (\d+), to 16.7 (\d+), to 25 (\d+)',l)
    if m: gpu.append((int(m.group(1)),int(m.group(2)),int(m.group(3))))
def s(n,a,skip=3):
    a=a[skip:] if len(a)>skip+2 else a
    if a: print("%-28s n=%2d median %7.2f  min %7.2f max %7.2f"%(n,len(a),st.median(a),min(a),max(a)))
s("fps (10 s samples)",fps,2)
s("frame avg ms",avg);s("frame p50",p50);s("frame p95",p95)
s("main waited for worker",mw);s("worker waited for work",ww)
s("worker ops ms",ops);s("shadowPackets ms",sp);s("present ms",pr);s("rtScene ms",rt)
if gpu: print("GPU time hist windows (<=12,<=16.7,<=25) last5:",gpu[-5:])
