'use strict';
document.title = 'SUMMIT EXTENSION LISTENING';

browser.test.onMessage.addListener(async (message, input) => {
    if (message !== 'summit-native-start') return;
    const assert = (condition, label) => {
        browser.test.assertTrue(!!condition, label);
        if (!condition) throw new Error(label);
    };
    const url = 'https://summit-cookie.invalid/';
    const name = 'summit-events-' + input.round + '-' + input.nonce;
    const persistentName = 'summit-reload-' + input.nonce;
    const events = [];
    const listener = change => {
        if (change.cookie && change.cookie.name === name) events.push(change);
    };
    const waitForEvents = async count => {
        const deadline = Date.now() + 10000;
        while (events.length < count && Date.now() < deadline)
            await new Promise(resolve => setTimeout(resolve, 20));
        if (events.length !== count)
            throw new Error('Expected ' + count + ' cookie events, received ' + events.length);
    };
    const details = value => ({url, name, value, path: '/', secure: true, httpOnly: true, sameSite: 'lax'});
    try {
        assert(browser.runtime.id === 'summit-runtime-fixture', 'cookie fixture runtime identity');
        const previous = await browser.cookies.get({url, name: persistentName});
        assert(input.round === 1 ? previous === null : previous && previous.value === input.nonce,
            'cookie persists through extension context destruction and recreation');
        assert(await browser.cookies.get({url, name}) === null, 'event cookie starts absent');

        browser.cookies.onChanged.addListener(listener);
        assert(browser.cookies.onChanged.hasListener(listener), 'cookie event listener is registered');
        const created = await browser.cookies.set(details('first-' + input.nonce));
        assert(created && created.name === name && created.value === 'first-' + input.nonce
            && created.domain === 'summit-cookie.invalid' && created.path === '/' && created.hostOnly
            && created.secure && created.httpOnly && created.session && created.sameSite === 'lax'
            && typeof created.storeId === 'string', 'cookie write returns the committed fields');
        await waitForEvents(1);
        assert(!events[0].removed && events[0].cause === 'explicit'
            && events[0].cookie.value === created.value && events[0].cookie.storeId === created.storeId,
            'insertion delivers a populated explicit addition event');

        const read = await new Promise((resolve, reject) => {
            chrome.cookies.get({url, name}, cookie => {
                const error = chrome.runtime.lastError;
                if (error) reject(new Error(error.message)); else resolve(cookie);
            });
        });
        assert(read && read.value === created.value && read.httpOnly && read.storeId === created.storeId,
            'Chrome callback reads the privileged HttpOnly cookie');
        const overwritten = await browser.cookies.set(details('second-' + input.nonce));
        assert(overwritten && overwritten.value === 'second-' + input.nonce,
            'overwrite returns its own committed receipt');
        await waitForEvents(3);
        assert(events[1].removed && events[1].cause === 'overwrite' && events[1].cookie.value === created.value
            && !events[2].removed && events[2].cause === 'explicit' && events[2].cookie.value === overwritten.value,
            'overwrite delivers ordered removal and addition events');
        const selected = await browser.cookies.getAll({url, name});
        assert(selected.length === 1 && selected[0].value === overwritten.value,
            'cookie query returns the overwritten value once');

        const removed = await browser.cookies.remove({url, name});
        assert(removed && removed.name === name && removed.url === url && removed.storeId === created.storeId,
            'cookie removal returns the matching store and identity');
        await waitForEvents(4);
        assert(events[3].removed && events[3].cause === 'explicit' && events[3].cookie.value === overwritten.value,
            'deletion delivers the actual removed cookie');
        assert(await browser.cookies.get({url, name}) === null, 'removed cookie is absent');

        browser.cookies.onChanged.removeListener(listener);
        await browser.cookies.set(details('after-listener-' + input.nonce));
        await browser.cookies.get({url, name});
        await new Promise(resolve => setTimeout(resolve, 250));
        assert(!browser.cookies.onChanged.hasListener(listener) && events.length === 4,
            'removed listener stays silent after a confirmed write and query');
        await browser.cookies.remove({url, name});

        if (input.round === 1)
            await browser.cookies.set({url, name: persistentName, value: input.nonce, path: '/', secure: true});
        else
            await browser.cookies.remove({url, name: persistentName});
        browser.test.sendMessage('summit-runtime-complete', {round: input.round, nonce: input.nonce});
        browser.test.notifyPass('summit-runtime-round-' + input.round);
        document.title = 'SUMMIT EXTENSION PASS ' + input.round + ' ' + input.nonce;
    } catch (error) {
        browser.cookies.onChanged.removeListener(listener);
        browser.test.notifyFail(String(error));
        document.title = 'SUMMIT EXTENSION FAIL ' + String(error);
    }
});
