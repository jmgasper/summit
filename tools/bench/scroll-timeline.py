#!/usr/bin/env python3
"""What a traced wheel scroll did, from a browser log with a time stamp at the
start of each line (seconds since launch, as tools/bench/run-scroll.py and the
test launchers write it).

    SUMMIT_WHEEL_ROUTE_TRACE=1 SUMMIT_COMPOSITOR_TIMING_TRACE=1 SUMMIT_SCROLL_LOCK_TRACE=1
    python3 tools/bench/scroll-timeline.py RUN_DIR

Prints the compositions of the scrolled page from the first notch that
scrolled to a quarter of a second after the last, the gaps between them, the
notches that the scrolling thread took over 20 ms to handle, and how long the
main thread kept the layers from hit tests. Compositions are timed by the
at= of their timing line, the compositor's own clock: the lines' arrival
times bunch by tens of milliseconds.
"""
import re, sys, collections
d=sys.argv[1]
ev=[]
for l in open(d+'/browser.log',errors='replace'):
    m=re.match(r'\s*([\d.]+) (.*)',l)
    if not m: continue
    ev.append((float(m.group(1)),m.group(2)))
ws=[e for e in ev if 'wheel route' in e[1]]
if not ws:
    print('  no wheel events'); sys.exit()
handled=[t for t,x in ev if 'wheel result' in x and 'handled=1' in x]
t0=handled[0] if handled else ws[0][0]; t1=ws[-1][0]
pids=collections.Counter(re.search(r'pid=(\d+)',x).group(1) for t,x in ev if 'compositor timing' in x and t0<t<t1)
pid=pids.most_common(1)[0][0]
raw=[(t,float(re.search(r'at=(\d+)',x).group(1))/1e6) for t,x in ev if 'compositor timing' in x and 'pid='+pid in x]
offset=min(t-a for t,a in raw)
frames=[a+offset for t,a in raw if t0<=a+offset<=t1+0.25]
gaps=[round((b-a)*1000) for a,b in zip(frames,frames[1:]) if b-a>0.040]
stalls=[]; wg=[]; prevW=None
for t,x in ev:
    if t<t0 or t>t1+0.3: continue
    m=re.search(r'wheel result.*took=([\d.]+)',x)
    if m and float(m.group(1))>20: stalls.append(round(float(m.group(1))))
    if 'wheel route' in x:
        if prevW is not None and t-prevW>0.075: wg.append(round((t-prevW)*1000))
        prevW=t
unhandled=sum(1 for t,x in ev if 'wheel result' in x and 'handled=0' in x and t>t0)
span=frames[-1]-frames[0] if len(frames)>1 else 1
print('  notches %d (first that scrolled at +%.2f s, %d unhandled after it); %d compositions, %.1f/s; gaps over 40 ms: %s; notch stalls: %s; late notches: %s'%(len(ws),t0-ws[0][0],unhandled,len(frames),(len(frames)-1)/span,gaps,stalls,wg))
holds=[float(re.search(r'hit tests ([\d.]+) ms',x).group(1)) for t,x in ev if 'kept the layers' in x and ws[0][0]-1.5<t<t1+0.5]
print('  main thread kept the layers over 20 ms: %d times, %.0f ms in all, longest %.0f'%(len(holds),sum(holds),max(holds or [0])))
