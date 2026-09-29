#!/usr/bin/env python3
"""The compositions of a page that plays a video, from a browser log with a
time stamp at the start of each line.

    SUMMIT_MSE_TRACE=1 SUMMIT_COMPOSITOR_TIMING_TRACE=1
    python3 tools/bench/video-compositions.py RUN_DIR

Prints, for the first five seconds of playback, the seven after them and the
rest: compositions a second, what one costs, how many took over 16.7 ms, and
the gaps over 40 ms. Timed by the at= of the timing lines.
"""
import re,collections,sys,statistics as st
d=sys.argv[1]
ev=[]
for l in open(d+'/browser.log',errors='replace'):
    m=re.match(r'\s*([\d.]+) (.*)',l)
    if m: ev.append((float(m.group(1)),m.group(2)))
first=[(t,x) for t,x in ev if 'first video frame' in x]
if not first: print('  no video frame'); sys.exit()
ft,fx=first[0]
nav=[t for t,x in ev if 'load player' in x][0]
fclock=float(re.search(r'\[([\d.]+)\]',fx).group(1))   # seconds of system_time
pids=collections.Counter(re.search(r'pid=(\d+)',x).group(1) for t,x in ev if 'compositor timing' in x)
pid=pids.most_common(1)[0][0]
fr=[]
for t,x in ev:
    if 'compositor timing' in x and 'pid='+pid in x:
        f=dict(re.findall(r'(\w+)=([\d.]+)',x)); f['t']=float(f['at'])/1e6-fclock
        if f['t']>0: fr.append(f)
span=float(sys.argv[2]) if len(sys.argv)>2 else 14
def part(a,b):
    p=[f for f in fr if a<=f['t']<b]
    if len(p)<2: return
    iv=[(y['t']-x['t'])*1000 for x,y in zip(p,p[1:])]
    gaps=[(round(x['t'],1),round((y['t']-x['t'])*1000)) for x,y in zip(p,p[1:]) if (y['t']-x['t'])>0.040]
    print('  %4.1f-%4.1f s: %.1f compositions/s, total %.1f ms (paint %.1f, read %.1f), draws %.0f, blendedMpx %.1f; over 16.7 ms %d of %d; intervals over 25 ms %d, over 40 ms %s'%(a,b,len(p)/(p[-1]['t']-p[0]['t']),st.mean(float(f['total']) for f in p),st.mean(float(f['paint']) for f in p),st.mean(float(f['read']) for f in p),st.mean(float(f['draws']) for f in p),st.mean(float(f['blendedMpx']) for f in p),sum(1 for f in p if float(f['total'])>16.7),len(p),sum(1 for i in iv if i>25),gaps))
print('  first frame %.2f s after the player; transparent tiles %d, uploaded %d'%(ft-nav,sum(int(f.get('transparentTiles',0)) for f in fr),sum(int(f['tiles']) for f in fr)))
part(0,5); part(5,12); part(12,100)
