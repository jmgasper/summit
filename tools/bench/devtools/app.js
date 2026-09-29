// What the page of tools/bench/devtools asks for and writes.
function show(text) {
    const out = document.getElementById('out');
    if (out) out.textContent += text + '\n';
}

async function ask() {
    const get = async (url, options) => {
        try {
            const response = await fetch(url, options);
            const body = await response.text();
            show(response.status + ' ' + url + ' ' + body.length + ' bytes');
        } catch (error) {
            show('failed ' + url + ': ' + error.message);
        }
    };
    await get('api/items.json?page=1');
    await get('api/text-json');
    await get('api/feed.xml');
    await get('api/fragment.html');
    await get('api/broken.json');
    await get('api/missing');
    await get('api/failing');
    await get('redirect');
    await get('api/bytes');
    await get('api/echo', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Asked-By': 'app.js' },
        body: JSON.stringify({ name: 'Summit', list: [1, 2, 3], nested: { ok: true } }) });
    await get('http://127.0.0.1:59999/refused');
    const request = new XMLHttpRequest();
    request.open('GET', 'api/items.json?from=xhr');
    request.onload = () => show(request.status + ' xhr ' + request.responseText.length + ' bytes');
    request.send();
    get('api/slow.json');
    get('api/large.json');
}

function write() {
    console.debug('debug: fine detail', { step: 1 });
    console.log('log: %s has %d items', 'list', 3, [1, 2, 3]);
    console.info('info: ready', new Map([['a', 1]]));
    console.warn('warning: deprecated call', { name: 'old', replacement: 'new' });
    console.error('error: something failed', new TypeError('not a function'));
    for (let i = 0; i < 3; ++i) console.log('repeated message');
    console.group('group');
    console.log('inside the group');
    console.groupEnd();
    console.log('an object', { id: 7, name: 'Ann', tags: ['a', 'b'], nested: { deep: true }, nothing: null, f() {} });
    console.assert(1 === 2, 'one is not two');
    console.log('several\nlines\nof text');
    (function traced() { console.trace('traced'); })();
}

window.addEventListener('DOMContentLoaded', () => {
    document.getElementById('again').addEventListener('click', ask);
    document.getElementById('log').addEventListener('click', write);
    document.getElementById('fail').addEventListener('click', () => { undefinedFunction(); });
    write();
    ask();
    setTimeout(() => { throw new Error('uncaught, from a timer'); }, 300);
});
