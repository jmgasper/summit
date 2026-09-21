'use strict';

// Exercises declarativeNetRequest and downloads when the background page
// loads, and again when the results popup asks. Each result is logged to the
// console as "[dnr-downloads] PASS|FAIL name" and the page title counts them.
const api = globalThis.browser ?? chrome;
let results = [];
let running = null;

function record(name, ok, detail) {
    results.push({name, ok: !!ok, detail});
    console.log(`[dnr-downloads] ${ok ? 'PASS' : 'FAIL'} ${name}${detail === undefined ? '' : ' ' + JSON.stringify(detail)}`);
    document.title = `DNR-DOWNLOADS ${results.filter(result => result.ok).length}/${results.length}`;
}

async function rejection(promise) {
    try {
        await promise;
        return null;
    } catch (error) {
        return String(error?.message ?? error);
    }
}

async function checkDeclarativeNetRequest() {
    const dnr = api.declarativeNetRequest;
    record('declarativeNetRequest namespace', typeof dnr === 'object');
    if (typeof dnr !== 'object')
        return;
    record('constants', dnr.MAX_NUMBER_OF_DYNAMIC_AND_SESSION_RULES > 0 && dnr.SESSION_RULESET_ID === '_session' && dnr.DYNAMIC_RULESET_ID === '_dynamic',
        {shared: dnr.MAX_NUMBER_OF_DYNAMIC_AND_SESSION_RULES, regex: dnr.MAX_NUMBER_OF_REGEX_RULES, guaranteed: dnr.GUARANTEED_MINIMUM_STATIC_RULES});

    const supported = await dnr.isRegexSupported({regex: '^https?://[a-z]+\\.test/'});
    record('isRegexSupported accepts a plain expression', supported.isSupported === true, supported);
    const backreference = await dnr.isRegexSupported({regex: '(a)\\1'});
    record('isRegexSupported rejects a backreference', backreference.isSupported === false && backreference.reason === 'syntaxError', backreference);

    // Start from a clean state; the same page may run the checks repeatedly.
    await dnr.updateSessionRules({removeRuleIds: (await dnr.getSessionRules()).map(rule => rule.id)});
    await dnr.updateDynamicRules({removeRuleIds: (await dnr.getDynamicRules()).map(rule => rule.id)});

    await dnr.updateSessionRules({addRules: [{id: 1, priority: 1, action: {type: 'block'},
        condition: {urlFilter: 'dnr-blocked', resourceTypes: ['xmlhttprequest', 'image']}}]});
    const session = await dnr.getSessionRules();
    record('session rule stored', session.length === 1 && session[0].id === 1, session);

    await dnr.updateDynamicRules({addRules: [{id: 10, priority: 2, action: {type: 'block'},
        condition: {urlFilter: 'dnr-dynamic-blocked', resourceTypes: ['xmlhttprequest']}}]});
    const dynamic = await dnr.getDynamicRules({ruleIds: [10]});
    record('dynamic rule stored', dynamic.length === 1 && dynamic[0].id === 10, dynamic);

    record('duplicate rule id rejected', !!await rejection(dnr.updateDynamicRules({addRules: [{id: 10, action: {type: 'block'}, condition: {urlFilter: 'x'}}]})));
    record('invalid argument throws synchronously', (() => {
        try {
            dnr.updateSessionRules({addRules: 'not an array'});
            return false;
        } catch {
            return true;
        }
    })());

    // The rule shape 1Password adds before a WebAuthn related-origins fetch.
    // Header modification is not enforced natively, so the promise must reject.
    const modifyHeaders = await rejection(dnr.updateSessionRules({addRules: [{id: 2, priority: 1,
        action: {type: 'modifyHeaders', requestHeaders: [{header: 'sec-fetch-site', operation: 'set', value: 'none'}],
            responseHeaders: [{header: 'access-control-allow-origin', operation: 'set', value: '*'}]},
        condition: {requestDomains: ['example.com'], urlFilter: '/.well-known/webauthn', resourceTypes: ['xmlhttprequest']}}]}));
    record('unsupported modifyHeaders rule rejects', !!modifyHeaders, modifyHeaders);
    record('rejected rule left no trace', (await dnr.getSessionRules()).length === 1);

    record('static ruleset disabled by default', !(await dnr.getEnabledRulesets()).includes('static_block'));
    await dnr.updateEnabledRulesets({enableRulesetIds: ['static_block']});
    record('static ruleset enabled', (await dnr.getEnabledRulesets()).includes('static_block'));
    const available = await dnr.getAvailableStaticRuleCount();
    record('getAvailableStaticRuleCount', typeof available === 'number' && available >= 0, available);
    record('unknown ruleset rejected', !!await rejection(dnr.updateEnabledRulesets({enableRulesetIds: ['missing']})));
}

async function checkDownloads() {
    const downloads = api.downloads;
    record('downloads namespace', typeof downloads === 'object');
    if (typeof downloads !== 'object')
        return;

    record('file URL rejected', !!await rejection(downloads.download({url: 'file:///boot/home/config/settings'})));
    record('parent path rejected', !!await rejection(downloads.download({url: 'data:text/plain,x', filename: '../x.txt'})));

    const created = [];
    const onCreated = item => created.push(item);
    downloads.onCreated.addListener(onCreated);

    // The 1Password export flow: a blob URL of this page, a suggested name,
    // then onChanged until the state is no longer in_progress.
    const blob = new Blob([`Summit downloads fixture ${new Date().toISOString()}\n`], {type: 'text/plain'});
    const url = URL.createObjectURL(blob);
    const id = await downloads.download({url, filename: 'summit-dnr-downloads-fixture.txt'});
    record('download() resolves with an id', Number.isInteger(id), id);
    const delta = await new Promise(resolve => {
        const timeout = setTimeout(() => resolve(null), 20000);
        const listener = change => {
            if (change.id !== id || !change.state || change.state.current === 'in_progress')
                return;
            clearTimeout(timeout);
            downloads.onChanged.removeListener(listener);
            resolve(change);
        };
        downloads.onChanged.addListener(listener);
    });
    URL.revokeObjectURL(url);
    downloads.onCreated.removeListener(onCreated);
    record('onCreated fired for the download', created.some(item => item.id === id && item.state === 'in_progress'), created);
    record('onChanged reports completion', delta?.state?.current === 'complete', delta);

    const [item] = await downloads.search({id});
    record('search finds the completed file', item?.state === 'complete' && /summit-dnr-downloads-fixture.*\.txt$/.test(item.filename)
        && item.byExtensionId === api.runtime.id && item.exists === true, item);
    record('search by query term', (await downloads.search({query: ['summit-dnr-downloads-fixture'], orderBy: ['-startTime'], limit: 1}))[0]?.id === id);

    const erased = await downloads.erase({id});
    record('erase removes history only', erased.includes(id) && (await downloads.search({id})).length === 0, erased);
    record('cancel of an erased download rejects', !!await rejection(downloads.cancel(id)));
}

async function run() {
    results = [];
    for (const check of [checkDeclarativeNetRequest, checkDownloads]) {
        try {
            await check();
        } catch (error) {
            record(`${check.name} threw`, false, String(error?.message ?? error));
        }
    }
    console.log(`[dnr-downloads] DONE ${results.filter(result => result.ok).length}/${results.length}`);
    return results;
}

api.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message?.op === 'run')
        running = run();
    Promise.resolve(running).then(() => sendResponse(results));
    return true;
});

running = run();
