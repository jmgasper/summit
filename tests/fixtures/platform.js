(async () => {
    const results = {};
    const assert = (condition, message) => { if (!condition) throw new Error(message); };
    const image = source => new Promise((resolve, reject) => {
        const element = new Image();
        element.onload = () => resolve(element);
        element.onerror = () => reject(new Error('Image decoding failed'));
        element.src = source;
    });
    const canvas = () => {
        const element = document.createElement('canvas');
        element.width = element.height = 16;
        document.querySelector('#fixtures').append(element);
        const context = element.getContext('2d');
        assert(context, '2D canvas context unavailable');
        return { element, context };
    };
    const pixelMatches = (context, x, y) => {
        const bytes = context.getImageData(x, y, 1, 1).data;
        return bytes[0] === 30 && bytes[1] === 144 && bytes[2] === 255 && bytes[3] === 255;
    };
    const probes = {
        secureContext: async () => assert(isSecureContext, 'Use the forwarded localhost fixture address'),
        canvasPixels: async () => {
            const { context } = canvas();
            context.fillStyle = '#1e90ff';
            context.fillRect(2, 2, 8, 8);
            assert(pixelMatches(context, 3, 3), 'Canvas pixel bytes differ from the requested color');
            assert(context.getImageData(0, 0, 1, 1).data[3] === 0, 'Untouched canvas pixel is not transparent');
        },
        canvasEncoding: async () => {
            const { element, context } = canvas();
            context.fillStyle = '#1e90ff';
            context.fillRect(0, 0, 16, 16);
            const encoded = element.toDataURL('image/png');
            assert(encoded.startsWith('data:image/png;base64,'), 'Canvas PNG encoding unavailable');
            const decoded = await image(encoded);
            const copy = canvas();
            copy.context.drawImage(decoded, 0, 0);
            assert(pixelMatches(copy.context, 5, 5), 'Canvas PNG round trip changed the pixels');
        },
        pngDecoding: async () => {
            const decoded = await image('/pixel.png');
            assert(decoded.naturalWidth === 1 && decoded.naturalHeight === 1, 'Single-pixel PNG dimensions are wrong');
            const { context } = canvas();
            context.drawImage(decoded, 0, 0);
            assert(pixelMatches(context, 0, 0), 'PNG decoder returned different pixels');
        },
        svgRendering: async () => {
            const decoded = await image('/square.svg');
            const { context } = canvas();
            context.drawImage(decoded, 0, 0);
            assert(pixelMatches(context, 4, 4), 'SVG did not render its filled rectangle');
        },
        fontMetrics: async () => {
            const { context } = canvas();
            context.font = '20px serif';
            const wide = context.measureText('mmmm').width;
            const narrow = context.measureText('iiii').width;
            assert(narrow > 0 && wide > narrow, 'Proportional font metrics are invalid');
        },
        shadowDOM: async () => {
            const host = document.createElement('div');
            document.querySelector('#fixtures').append(host);
            const shadow = host.attachShadow({ mode: 'open' });
            shadow.innerHTML = '<style>span{display:inline-block;width:37px}</style><span>Shadow</span>';
            assert(shadow.querySelector('span').getBoundingClientRect().width === 37, 'Shadow-root styles were not applied');
        },
        modules: async () => {
            const module = await import('/module.js');
            assert(module.answer === 42 && module.value() === 'module loaded', 'Module import returned incorrect exports');
        },
        workerTransfer: () => new Promise((resolve, reject) => {
            const worker = new Worker('/worker.js');
            const timer = setTimeout(() => { worker.terminate(); reject(new Error('Worker timed out')); }, 7000);
            const finish = () => { clearTimeout(timer); worker.terminate(); };
            worker.onerror = event => { finish(); reject(new Error(event.message)); };
            worker.onmessage = event => {
                finish();
                try { assert(event.data === 24, 'Worker received incorrect transferred bytes'); resolve(); }
                catch (error) { reject(error); }
            };
            const bytes = new Uint8Array([7, 8, 9]);
            worker.postMessage(bytes.buffer, [bytes.buffer]);
            if (bytes.byteLength !== 0) { finish(); reject(new Error('Transferred buffer was not detached')); }
        }),
        indexedDB: () => new Promise((resolve, reject) => {
            const name = 'summit-platform-' + Date.now() + '-' + Math.random();
            const request = indexedDB.open(name, 1);
            request.onerror = () => reject(request.error);
            request.onupgradeneeded = () => request.result.createObjectStore('values');
            request.onsuccess = () => {
                const database = request.result;
                const transaction = database.transaction('values', 'readwrite');
                transaction.objectStore('values').put('persisted value', 'key');
                transaction.onerror = () => { database.close(); reject(transaction.error); };
                transaction.oncomplete = () => {
                    const read = database.transaction('values').objectStore('values').get('key');
                    read.onerror = () => { database.close(); reject(read.error); };
                    read.onsuccess = () => {
                        const correct = read.result === 'persisted value';
                        database.close();
                        indexedDB.deleteDatabase(name);
                        if (correct) resolve(); else reject(new Error('IndexedDB returned different data'));
                    };
                };
            };
        }),
        webCrypto: async () => {
            const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode('abc'));
            const hex = Array.from(new Uint8Array(digest), byte => byte.toString(16).padStart(2, '0')).join('');
            assert(hex === 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad', 'SHA-256 returned an incorrect digest');
        },
        broadcastChannel: () => new Promise((resolve, reject) => {
            const name = 'summit-channel-' + Math.random();
            const sender = new BroadcastChannel(name), receiver = new BroadcastChannel(name);
            const timer = setTimeout(() => { sender.close(); receiver.close(); reject(new Error('BroadcastChannel timed out')); }, 7000);
            receiver.onmessage = event => {
                clearTimeout(timer); sender.close(); receiver.close();
                if (event.data === 'channel message') resolve(); else reject(new Error('Incorrect broadcast message'));
            };
            sender.postMessage('channel message');
        }),
    };
    await Promise.all(Object.entries(probes).map(async ([name, probe]) => {
        let timer;
        try {
            await Promise.race([probe(), new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('Check timed out')), 8000); })]);
            results[name] = { passed: true };
        } catch (error) {
            results[name] = { passed: false, error: String(error) };
        } finally { clearTimeout(timer); }
    }));
    const report = { suite: 'platform', origin: location.origin, userAgent: navigator.userAgent, results };
    document.querySelector('#results').textContent = JSON.stringify(report, null, 2);
    document.title = Object.values(results).every(result => result.passed) ? 'Summit platform PASS' : 'Summit platform FAIL';
    await fetch('/report', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(report) });
})().catch(error => { document.title = 'Summit platform ERROR'; document.querySelector('#results').textContent = String(error); });
