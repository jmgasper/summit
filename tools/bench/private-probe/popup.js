chrome.tabs.query({ active: true, currentWindow: true }, tabs => {
  const tab = tabs[0];
  document.getElementById('where').textContent = tab ? (tab.incognito ? 'Private tab' : 'Normal tab') + ': ' + tab.url : 'No tab';
  console.log('probe popup', tab && tab.incognito, tab && tab.url);
});
