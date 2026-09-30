document.documentElement.dataset.summitProbe = chrome.extension.inIncognitoContext ? 'private' : 'normal';
chrome.runtime.sendMessage({ url: location.href }, reply => {
  document.documentElement.dataset.summitProbeCount = String(reply && reply.count);
});
