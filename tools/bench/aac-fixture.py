#!/usr/bin/env python3
"""Generate AAC-LC/MPEG-2 AAC fixtures and serve native audio playback checks."""
import argparse
import http.server
import json
import pathlib
import subprocess
import urllib.parse

CASES = [
    ('lc.m4a', 'audio/m4a; codecs="mp4a.40.2"'),
    ('lc.m4a', 'audio/x-m4a; codecs="mp4a.40.2"'),
    ('lc.m4a', 'audio/mp4; codecs="mp4a.40.2"'),
    ('mpeg2.m4a', 'audio/mp4; codecs="mp4a.67"'),
    ('mpeg2.m4a', 'audio/m4a; codecs="mp4a.67"'),
    ('lc.aac', 'audio/aac'),
    ('lc.aac', 'audio/x-aac'),
    ('mpeg2.aac', 'audio/aac'),
    ('mpeg2.aac', 'audio/x-aac'),
    ('control.mp3', 'audio/mpeg'),
    ('control.ogg', 'audio/ogg; codecs="vorbis"'),
    ('control.wav', 'audio/wav'),
]

PAGE = '''<!doctype html><meta charset="utf-8"><title>AAC starting</title>
<style>body{font:16px sans-serif}button{position:absolute;left:20px;top:20px;width:200px;height:50px}audio{margin-top:90px}</style>
<button id="play">Play AAC test</button><audio id="audio" controls></audio><pre id="status"></pre>
<script>
const cases=CASES, index=Number(new URLSearchParams(location.search).get('case'));
const [file,type]=cases[index], audio=document.getElementById('audio');
const checks=[], events=[];
const state=()=>({index,file,type,time:audio.currentTime,duration:audio.duration,ready:audio.readyState,
  error:audio.error&&{code:audio.error.code,message:audio.error.message},checks,events});
const assert=(value,label)=>{checks.push({label,passed:!!value});if(!value)throw Error(label)};
const wait=async(predicate,label)=>{const end=performance.now()+15000;while(!predicate()){
 if(audio.error)throw Error(JSON.stringify(state().error));if(performance.now()>end)throw Error(label);await new Promise(r=>setTimeout(r,50))}};
const report=async(prefix)=>{const result=state();document.getElementById('status').textContent=JSON.stringify(result,null,2);
 await fetch('/result/'+index,{method:'POST',body:JSON.stringify(result)});
 document.title=prefix+' '+index+' '+JSON.stringify(result.checks.filter(c=>!c.passed))};
for(const name of ['loadedmetadata','loadeddata','playing','pause','seeking','seeked','ended','error'])
 audio.addEventListener(name,()=>events.push({name,time:audio.currentTime,at:performance.now()}));
document.getElementById('play').onclick=async()=>{try{
 assert(audio.canPlayType(type)!=='','advertise selected AAC type');
 for(const mime of ['audio/aac','audio/x-aac','audio/m4a','audio/x-m4a']){
  assert(audio.canPlayType(mime)!=='','advertise '+mime);
  assert(audio.canPlayType(mime+'; codecs="not-a-codec"')==='','reject unknown codec in '+mime);
  assert(MediaSource.isTypeSupported(mime)===(mime==='audio/aac'),'canonical ADTS MSE type, file aliases remain separate '+mime);
 }
 const source=document.createElement('source');source.src='/media/'+file+'?type='+encodeURIComponent(type);source.type=type;
 audio.replaceChildren(source);audio.load();await audio.play();const playStarted=performance.now();
 await wait(()=>audio.currentTime>.5,'clock advances');
 assert(!audio.ended&&audio.currentTime<1.2,'clock advances smoothly');
 assert(performance.now()-playStarted>150&&performance.now()-playStarted<2000,'audio clock tracks elapsed playback');
 assert(!audio.muted&&audio.volume===1,'audio output enabled');
 assert(audio.duration>=4.9&&audio.duration<5.5,'finite duration');
 audio.pause();const before=audio.currentTime;events.push({name:'paused-check-start',time:before,at:performance.now()});await new Promise(r=>setTimeout(r,300));
 events.push({name:'paused-check-end',time:audio.currentTime,at:performance.now()});
 assert(Math.abs(audio.currentTime-before)<.08,'pause holds clock');
 audio.currentTime=2.5;await wait(()=>!audio.seeking,'seek completes');
 assert(Math.abs(audio.currentTime-2.5)<.15,'seek reaches target');await audio.play();
 await wait(()=>audio.ended,'playback reaches end');assert(audio.currentTime>=4.9,'complete AAC playback');
 assert(events.some(e=>e.name==='seeked')&&events.some(e=>e.name==='ended'),'seeked and ended events');
 report('AAC_PASS');
}catch(error){checks.push({label:String(error),passed:false});report('AAC_FAIL')}};
document.title='AAC ready '+index;
</script>'''.replace('CASES', json.dumps(CASES))


def generate(directory):
    directory.mkdir(parents=True, exist_ok=True)
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi',
        '-i', 'sine=frequency=523:sample_rate=48000:duration=5', '-af', 'volume=0.1',
        '-c:a', 'aac', '-profile:a', 'aac_low', '-aac_pns', '0', '-b:a', '96k',
        str(directory / 'lc.m4a')], check=True)
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-i',
        str(directory / 'lc.m4a'), '-c:a', 'copy', '-f', 'adts', str(directory / 'lc.aac')], check=True)
    # Disable MPEG-4 perceptual noise substitution when encoding above.
    # MPEG-2 AAC-LC then uses the same LC payload, identified by ADTS's ID bit or
    # MPEG-4's ES_Descriptor objectTypeIndication 0x67 (ISO/IEC 13818-7).
    adts = bytearray((directory / 'lc.aac').read_bytes())
    at = 0
    while at < len(adts):
        assert adts[at] == 255 and adts[at+1] & 0xf6 == 0xf0 and adts[at+2] >> 6 == 1
        size = ((adts[at+3] & 3) << 11) | (adts[at+4] << 3) | (adts[at+5] >> 5)
        assert size >= 7 and at + size <= len(adts)
        adts[at+1] |= 8
        at += size
    (directory / 'mpeg2.aac').write_bytes(adts)
    mp4 = bytearray((directory / 'lc.m4a').read_bytes())
    esds = mp4.index(b'esds')
    descriptor = mp4.index(b'\x04\x80\x80\x80', esds)
    assert descriptor < esds + 24 and mp4[descriptor+5] == 0x40
    mp4[descriptor+5] = 0x67
    (directory / 'mpeg2.m4a').write_bytes(mp4)
    # The PCM-based clock is shared by file playback; exercise unchanged
    # formats as well as AAC so seeking and encoder delay remain covered.
    for filename, codec in [('control.mp3', 'libmp3lame'), ('control.ogg', 'libvorbis'), ('control.wav', 'pcm_s16le')]:
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi',
            '-i', 'sine=frequency=523:sample_rate=48000:duration=5', '-af', 'volume=0.1',
            '-c:a', codec, str(directory / filename)], check=True)


def serve(directory, host, port):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            if not self.path.startswith('/result/') or self.path[8:] not in {str(i) for i in range(len(CASES))}:
                self.send_error(404); return
            size = int(self.headers.get('Content-Length', 0))
            if size > 100000:
                self.send_error(400); return
            result = json.loads(self.rfile.read(size))
            (directory / ('result-' + self.path[8:] + '.json')).write_text(json.dumps(result, indent=2) + '\n')
            self.send_response(204); self.end_headers()

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            if url.path == '/test.html':
                data, mime = PAGE.encode(), 'text/html; charset=utf-8'
            elif url.path.startswith('/media/') and url.path[7:] in {c[0] for c in CASES}:
                data = (directory / url.path[7:]).read_bytes()
                mime = urllib.parse.parse_qs(url.query).get('type', ['audio/mp4'])[0]
                if mime not in {c[1] for c in CASES}:
                    self.send_error(400); return
            else:
                self.send_error(404); return
            self.send_response(200)
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass
    http.server.ThreadingHTTPServer((host, port), Handler).serve_forever()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=pathlib.Path, default=pathlib.Path('.vm/aac-fixture'))
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=8775)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    generate(args.directory)
    if not args.generate_only:
        serve(args.directory, args.host, args.port)
