#!/usr/bin/env python3
"""Generate H.264/AAC DASH media and serve native playback/network fixtures."""
import argparse
import copy
import datetime
import http.server
import json
import pathlib
import re
import subprocess
import threading
import time
import xml.etree.ElementTree as ET
from urllib.parse import urlsplit

NS = '{urn:mpeg:dash:schema:mpd:2011}'
ET.register_namespace('', NS[1:-1])
PAGE = '''<!doctype html><meta charset="utf-8"><title>DASH fixture ready</title>
<style>body{font:16px sans-serif;margin:20px}video{display:block;width:640px;height:360px;background:#000}button{margin:12px 0;padding:12px}pre{white-space:pre-wrap}</style>
<h1>Summit DASH playback</h1><video id="video" controls playsinline></video>
<button id="play">Play with audio</button><pre id="status"></pre><script>
const video=document.getElementById('video');let events=[],widths=[];
const run=new URLSearchParams(location.search).get('run')||'manual';
for(const name of ['loadedmetadata','loadeddata','canplay','playing','waiting','seeking','seeked','ended','error','pause','resize'])
 video.addEventListener(name,()=>{events.push({name,time:video.currentTime,width:video.videoWidth});if(video.videoWidth&&widths.at(-1)!==video.videoWidth)widths.push(video.videoWidth)});
const ranges=r=>Array.from({length:r.length},(_,i)=>[r.start(i),r.end(i)]);
window.dashTest={
 load(mode){video.pause();video.removeAttribute('src');video.load();events=[];widths=[];video.muted=true;video.src='/'+run+'-'+mode+'-'+Date.now()+'/'+mode+'/manifest.mpd';video.load();return true},
 loadMSE(){video.pause();video.removeAttribute('src');video.load();events=[];widths=[];video.muted=true;window.mseError=null;const media=new MediaSource();const blob=URL.createObjectURL(media);video.src=blob;media.addEventListener('sourceopen',async()=>{URL.revokeObjectURL(blob);try{await Promise.all([[1,'video/mp4; codecs="avc1.64001e"'],[2,'audio/mp4; codecs="mp4a.40.2"']].map(async([id,type])=>{const buffer=media.addSourceBuffer(type);const data=await fetch('/'+run+'-mse/fast/range-stream'+id+'.mp4').then(r=>r.arrayBuffer());await new Promise((resolve,reject)=>{buffer.addEventListener('updateend',resolve,{once:true});buffer.addEventListener('error',reject,{once:true});buffer.appendBuffer(data)})}));media.endOfStream()}catch(e){window.mseError=String(e)}} ,{once:true});return true},
 play(){return video.play().then(()=>true,e=>e.name)},
 state(){return {time:video.currentTime,duration:Number.isFinite(video.duration)?video.duration:String(video.duration),ready:video.readyState,paused:video.paused,ended:video.ended,seeking:video.seeking,width:video.videoWidth,height:video.videoHeight,buffered:ranges(video.buffered),seekable:ranges(video.seekable),quality:video.getVideoPlaybackQuality().totalVideoFrames,error:video.error&&{code:video.error.code,message:video.error.message},events,widths}},
 pixels(){const c=document.createElement('canvas');c.width=64;c.height=36;const x=c.getContext('2d');x.drawImage(video,0,0,64,36);const a=x.getImageData(0,0,64,36).data;let sum=0,colored=0;for(let i=0;i<a.length;i+=4){sum=(sum+a[i]*3+a[i+1]*5+a[i+2]*7)>>>0;if(Math.max(a[i],a[i+1],a[i+2])-Math.min(a[i],a[i+1],a[i+2])>50)colored++}return {sum,colored}},
 clear(){video.pause();video.removeAttribute('src');video.load();return true}
};
document.getElementById('play').onclick=()=>{video.muted=false;video.play()};
setInterval(()=>document.getElementById('status').textContent=JSON.stringify(dashTest.state(),null,2),500);
</script>'''


def generate(directory):
    directory.mkdir(parents=True, exist_ok=True)
    if not (directory / 'manifest.mpd').is_file():
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
            '-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=30',
            '-f', 'lavfi', '-i', 'sine=frequency=523:sample_rate=48000', '-t', '18',
            '-map', '0:v', '-map', '0:v', '-map', '1:a', '-filter:v:0', 'scale=320:180',
            '-c:v', 'libx264', '-preset', 'veryfast', '-pix_fmt', 'yuv420p', '-g', '60',
            '-keyint_min', '60', '-sc_threshold', '0', '-b:v:0', '280k', '-b:v:1', '850k',
            '-c:a', 'aac', '-b:a', '96k', '-f', 'dash', '-seg_duration', '2',
            '-use_template', '1', '-use_timeline', '1', '-adaptation_sets', 'id=0,streams=v id=1,streams=a',
            str(directory / 'manifest.mpd')], check=True)
    root = ET.parse(directory / 'manifest.mpd').getroot()
    ranged = copy.deepcopy(root)
    for rep in ranged.iter(NS + 'Representation'):
        identifier = rep.attrib['id']
        template = rep.find(NS + 'SegmentTemplate')
        initialization = (directory / f'init-stream{identifier}.m4s').read_bytes()
        output = bytearray(initialization)
        segment_list = ET.Element(NS + 'SegmentList', {key: value for key, value in template.attrib.items()
            if key in ('timescale', 'presentationTimeOffset', 'duration', 'startNumber')})
        filename = f'range-stream{identifier}.mp4'
        ET.SubElement(segment_list, NS + 'Initialization', sourceURL=filename, range=f'0-{len(output)-1}')
        segment_list.append(copy.deepcopy(template.find(NS + 'SegmentTimeline')))
        for fragment in sorted(directory.glob(f'chunk-stream{identifier}-*.m4s')):
            first = len(output); output.extend(fragment.read_bytes())
            ET.SubElement(segment_list, NS + 'SegmentURL', media=filename, mediaRange=f'{first}-{len(output)-1}')
        (directory / filename).write_bytes(output)
        rep.remove(template); rep.append(segment_list)
    (directory / 'range.mpd').write_bytes(ET.tostring(ranged, encoding='utf-8', xml_declaration=True))


def serve(directory, host, port):
    lock = threading.Lock(); runs = {}
    class Handler(http.server.BaseHTTPRequestHandler):
        def reply(self, code, data, mime, headers=None, slow=False):
            self.send_response(code); self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(data))); self.send_header('Cache-Control', 'no-store')
            for key, value in (headers or {}).items(): self.send_header(key, value)
            self.end_headers()
            try:
                for at in range(0, len(data), 8192):
                    block = data[at:at+8192]
                    if slow: time.sleep(len(block) / 22000)
                    self.wfile.write(block); self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError): pass

        def do_GET(self):
            path = urlsplit(self.path).path
            if path == '/test.html': return self.reply(200, PAGE.encode(), 'text/html')
            if path == '/policy.html': return self.reply(200, PAGE.encode(), 'text/html', {'Content-Security-Policy': "media-src 'none'"})
            if path == '/session': return self.reply(200, b'<!doctype html><title>DASH session ready</title>', 'text/html', {'Set-Cookie': 'summit-dash=yes; HttpOnly; SameSite=Strict; Path=/'})
            if path.startswith('/metrics/'):
                with lock: data = json.dumps(runs.get(path.split('/')[-1], {})).encode()
                return self.reply(200, data, 'application/json')
            match = re.fullmatch(r'/([a-zA-Z0-9-]+)/(fast|adaptive|live|range|bad-range|bad|auth|missing)/([a-zA-Z0-9_.-]+)', path)
            if not match: return self.reply(404, b'Not found', 'text/plain')
            run, mode, filename = match.groups()
            with lock:
                state = runs.setdefault(run, {'started': time.time(), 'requests': []})
                state['requests'].append({'at': time.time(), 'path': path, 'range': self.headers.get('Range'), 'cookie': bool(self.headers.get('Cookie'))})
                state['requests'] = state['requests'][-10000:]
                start = state['started']
            if mode == 'auth' and 'summit-dash=yes' not in self.headers.get('Cookie', ''): return self.reply(403, b'Cookie required', 'text/plain')
            if mode == 'bad': return self.reply(200, b'<MPD><broken>', 'application/dash+xml')
            if mode == 'missing' and filename.startswith('chunk-'): return self.reply(404, b'Missing segment', 'text/plain')
            if filename == 'manifest.mpd':
                data = (directory / ('range.mpd' if mode in ('range', 'bad-range') else 'manifest.mpd')).read_bytes()
                if mode == 'live':
                    root = ET.fromstring(data); root.attrib['type'] = 'dynamic'; root.attrib.pop('mediaPresentationDuration', None)
                    root.attrib.update(availabilityStartTime=datetime.datetime.fromtimestamp(start - 8, datetime.timezone.utc).isoformat().replace('+00:00', 'Z'), minimumUpdatePeriod='PT1S', timeShiftBufferDepth='PT8S')
                    data = ET.tostring(root, encoding='utf-8')
                return self.reply(200, data, 'application/dash+xml')
            file = directory / filename
            if not file.is_file() or file.suffix not in ('.m4s', '.mp4'): return self.reply(404, b'Not found', 'text/plain')
            data = file.read_bytes(); headers = {}; code = 200
            if value := self.headers.get('Range'):
                byte_range = re.fullmatch(r'bytes=(\d+)-(\d+)', value)
                if not byte_range: return self.reply(416, b'Bad range', 'text/plain')
                first, last = map(int, byte_range.groups())
                if first > last or last >= len(data): return self.reply(416, b'Bad range', 'text/plain')
                headers['Content-Range'] = f'bytes {first + (1 if mode == "bad-range" else 0)}-{last}/{len(data)}'
                data = data[first:last+1]; code = 206
            self.reply(code, data, 'video/mp4', headers, mode == 'adaptive' and time.time() - start > 3)
    http.server.ThreadingHTTPServer((host, port), Handler).serve_forever()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--directory', type=pathlib.Path, default=pathlib.Path('.vm/issue33-fixture'))
    parser.add_argument('--host', default='0.0.0.0'); parser.add_argument('--port', type=int, default=8772)
    parser.add_argument('--generate-only', action='store_true'); args = parser.parse_args()
    generate(args.directory)
    if not args.generate_only: serve(args.directory, args.host, args.port)
