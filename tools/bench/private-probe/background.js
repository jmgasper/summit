// One background page for every window: the count goes on across normal and
// private windows when the extension is allowed in both.
let count = 0;
chrome.runtime.onMessage.addListener((message, sender, reply) => {
  count += 1;
  console.log(`probe seen ${message.url} incognito=${sender.tab ? sender.tab.incognito : '?'} count=${count}`);
  reply({ count });
});
chrome.tabs.onCreated.addListener(tab => console.log(`probe tab created incognito=${tab.incognito}`));
chrome.windows.onCreated.addListener(window => console.log(`probe window created incognito=${window.incognito}`));
