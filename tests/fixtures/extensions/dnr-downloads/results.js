'use strict';
const api = globalThis.browser ?? chrome;
async function show(op) {
    const list = document.getElementById('results');
    list.textContent = 'running…';
    const results = await api.runtime.sendMessage({op});
    list.textContent = '';
    for (const result of results) {
        const item = document.createElement('li');
        item.className = result.ok ? 'pass' : 'fail';
        item.textContent = `${result.ok ? 'PASS' : 'FAIL'} ${result.name}`;
        if (result.detail !== undefined) {
            const detail = document.createElement('pre');
            detail.textContent = JSON.stringify(result.detail, null, 1);
            item.append(detail);
        }
        list.append(item);
    }
}
document.getElementById('run').addEventListener('click', () => show('run'));
show('results');
