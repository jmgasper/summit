#!/usr/bin/env python3
import json, collections
B='/mnt/HaikuWork/apps/summit/.cache/ext-survey/'
inv=json.load(open(B+'inventory.json')); surf=json.load(open(B+'haiku-surface.json'))['surface']
# nested/ok members that the IDL exposes via sub-objects
extra_ok={'storage':{'local','session','sync','onChanged'},'runtime':{'lastError','id'},'privacy':{'network','services','websites'}}
out={}
for n,v in inv.items():
    miss_ns=collections.Counter(); miss_mem=collections.Counter()
    for k,c in v['api_refs'].items():
        ns,mem=k.split('.')
        if ns not in surf: miss_ns[ns]+=c; continue
        if mem not in surf[ns] and mem not in extra_ok.get(ns,set()) and not mem[0].isupper():
            miss_mem[k]+=c
    out[n]={'missing_namespaces':sorted(miss_ns),'missing_members':sorted(miss_mem)}
    print(f"{n}: NS-missing={','.join(sorted(miss_ns))}\n    members-missing={','.join(sorted(miss_mem))}")
json.dump(out,open(B+'static-gaps.json','w'),indent=1)
