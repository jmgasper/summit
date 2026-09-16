var packagedCount = (typeof packagedCount === "number" ? packagedCount : 0) + 1;
({count: packagedCount, id: browser.runtime.id, label: document.documentElement.dataset.label});
