'use strict';

const events = [];
const held = {};
const paths = {
    network: ['network', 'networkPredictionEnabled'],
    hyperlink: ['websites', 'hyperlinkAuditingEnabled'],
    rtc: ['network', 'webRTCIPHandlingPolicy'],
};
const pause = () => new Promise(resolve => setTimeout(resolve, 150));

try {
    for (const [name, [category, property]] of Object.entries(paths)) {
        const setting = browser.privacy?.[category]?.[property];
        if (!setting) continue;
        held[name] = setting;
        setting.onChange.addListener(details => events.push({setting: name, details}));
    }
    browser.test.onMessage.addListener(async (message, input) => {
        if (message !== 'summit-privacy-command') return;
        const reply = {sequence: input.sequence, nonce: input.nonce, extension: browser.runtime.id, ok: true};
        try {
            if (input.op === 'probe') {
                reply.result = {
                    hasPrivacy: typeof browser.privacy === 'object',
                    aliases: typeof chrome.privacy === 'object' && typeof browser.privacy === 'object',
                    held: Object.keys(held).sort(),
                };
            } else if (input.op === 'events') {
                await pause();
                reply.result = events.splice(0);
            } else {
                const [category, property] = paths[input.setting];
                const setting = input.held ? held[input.setting] : browser.privacy[category][property];
                if (input.mode === 'callback') {
                    let callbacks = 0;
                    let callbackError;
                    let callbackValue;
                    await new Promise(resolve => {
                        setting[input.op](input.details, value => {
                            ++callbacks;
                            callbackError = chrome.runtime.lastError?.message;
                            callbackValue = value;
                            resolve();
                        });
                    });
                    await pause();
                    reply.callbacks = callbacks;
                    reply.lastErrorCleared = chrome.runtime.lastError === undefined;
                    if (callbacks !== 1 || !reply.lastErrorCleared)
                        throw new Error('Callback count or runtime.lastError lifetime is invalid');
                    if (callbackError) throw new Error(callbackError);
                    reply.result = callbackValue === undefined ? null : callbackValue;
                } else {
                    const promise = setting[input.op](input.details);
                    if (!promise || typeof promise.then !== 'function')
                        throw new Error('Omitting the callback did not return a Promise');
                    const result = await promise;
                    reply.result = result === undefined ? null : result;
                }
            }
        } catch (error) {
            reply.ok = false;
            reply.error = String(error?.message || error);
        }
        await pause();
        browser.test.sendMessage('summit-privacy-response', reply);
        document.title = 'SUMMIT PRIVACY ' + input.nonce + ' ' + input.sequence + ' ' + JSON.stringify(reply);
    });
    document.title = 'SUMMIT PRIVACY LISTENING';
} catch (error) {
    document.title = 'SUMMIT PRIVACY FAIL ' + String(error);
}
