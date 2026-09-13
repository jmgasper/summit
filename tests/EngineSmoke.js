// Run with the pinned engine's jsc shell. This is a port smoke test, not a
// replacement for Test262, WebKit's JSC stress tests or browser conformance.
let checks = 0;
function check(value, description) {
    if (!value) throw new Error(description);
    ++checks;
    print('PASS ' + description);
}

async function main() {
    check(2n ** 100n === 1267650600228229401496703205376n, 'BigInt arithmetic');
    check([3, 1, 2].toSorted().join(',') === '1,2,3', 'copying array methods');
    const groups = Object.groupBy([1, 2, 3], x => x % 2);
    check(groups[0].join(',') === '2' && groups[1].join(',') === '1,3', 'Object.groupBy');
    class Counter { #value = 0; next() { return ++this.#value; } }
    const counter = new Counter();
    check(counter.next() === 1 && counter.next() === 2, 'private class fields');
    const view = new DataView(new ArrayBuffer(8));
    view.setBigUint64(0, 0x0102030405060708n);
    check(view.getUint8(0) === 1 && view.getBigUint64(0) === 0x0102030405060708n, 'typed buffer access');
    check(/\p{Letter}+/u.test('café'), 'Unicode regular expressions');
    check(/[\p{Letter}&&\p{ASCII}]+/v.test('abc'), 'Unicode set regular expressions');
    check(new Intl.ListFormat('en-US').format(['A', 'B', 'C']) === 'A, B, and C', 'ICU list formatting');
    check(new Intl.NumberFormat('en-US').format(12345.5) === '12,345.5', 'ICU number formatting');
    check(new Intl.DateTimeFormat('en-US', { timeZone: 'UTC', dateStyle: 'short' })
        .format(new Date(Date.UTC(2024, 0, 2))) === '1/2/24', 'ICU date formatting');
    check([...new Intl.Segmenter('en', { granularity: 'word' }).segment('hello world')]
        .filter(part => part.isWordLike).length === 2, 'ICU word segmentation');
    check('e\u0301'.normalize('NFC') === 'é', 'Unicode normalization');
    check(await Promise.resolve(42) === 42, 'Promise microtasks and async functions');
    const settled = await Promise.allSettled([Promise.resolve(1), Promise.reject('expected')]);
    check(settled[0].value === 1 && settled[1].status === 'rejected', 'Promise settlement');
    function hot(value) { return value * 3 + 1; }
    let sum = 0;
    for (let i = 0; i < 100000; ++i) sum += hot(i);
    check(sum === 14999950000, 'hot JavaScript execution');
    if (!globalThis.summitJavaScriptOnly) {
        const bytes = new Uint8Array([0,97,115,109,1,0,0,0,1,5,1,96,0,1,127,
            3,2,1,0,7,10,1,6,97,110,115,119,101,114,0,0,10,6,1,4,0,65,42,11]);
        const wasm = new WebAssembly.Instance(new WebAssembly.Module(bytes));
        check(wasm.exports.answer() === 42, 'WebAssembly compilation and execution');
    }
    print(checks + ' checks, 0 failures');
}

asyncTestStart(1);
main().then(() => asyncTestPassed(), error => { print('FAIL ' + error.stack); quit(1); });
