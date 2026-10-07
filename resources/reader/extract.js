// Runs in a fresh WebKit content world, never the website's JavaScript world.
// Readability and DOMPurify are loaded in that same world before this function.
(() => {
    const source = new URL(document.URL);
    if (!['http:', 'https:'].includes(source.protocol)
        || !['text/html', 'application/xhtml+xml'].includes(document.contentType)) return '';
    const article = new Readability(document.cloneNode(true), {
        maxElemsToParse: 50000, charThreshold: 300, keepClasses: false,
        serializer: element => element
    }).parse();
    if (!article || article.length < 300) return '';
    // Resolve every URL before sanitizing. Only ordinary web links and images
    // survive; embedded documents, active content and form controls do not.
    for (const node of article.content.querySelectorAll('[href],[src],[srcset]')) {
        node.removeAttribute('srcset');
        for (const attr of ['href', 'src']) {
            if (!node.hasAttribute(attr)) continue;
            try {
                const url = new URL(node.getAttribute(attr), source);
                if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password)
                    node.removeAttribute(attr);
                else node.setAttribute(attr, url.href);
            } catch { node.removeAttribute(attr); }
        }
    }
    const clean = DOMPurify.sanitize(article.content, {
        ALLOWED_TAGS: ['p','div','section','article','h1','h2','h3','h4','h5','h6',
            'blockquote','pre','code','strong','b','em','i','u','s','del','small',
            'sub','sup','br','hr','ul','ol','li','dl','dt','dd','a','img',
            'figure','figcaption','table','caption','thead','tbody','tfoot','tr','th','td',
            'abbr','time','span','mark','ruby','rt','rp'],
        ALLOWED_ATTR: ['href','src','alt','title','colspan','rowspan','scope','dir','lang','start'],
        ALLOW_DATA_ATTR: false, ALLOW_ARIA_ATTR: false,
        ALLOWED_URI_REGEXP: /^https?:\/\//i,
        RETURN_DOM_FRAGMENT: true
    });
    for (const link of clean.querySelectorAll('a')) link.setAttribute('rel', 'noreferrer noopener');
    for (const img of clean.querySelectorAll('img')) {
        img.setAttribute('loading', 'lazy');
        img.setAttribute('referrerpolicy', 'no-referrer');
    }
    const container = document.createElement('div'); container.appendChild(clean);
    if (container.innerHTML.length > 3000000) return '';
    const escape = value => String(value || '').replace(/[&<>"']/g,
        c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const title = escape(article.title || document.title);
    const meta = [article.byline, article.publishedTime].filter(Boolean).map(escape).join(' · ');
    const dir = article.dir === 'rtl' ? 'rtl' : 'ltr';
    const html = `<!doctype html><html dir="${dir}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src https: http:; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-src 'none'">
<title>${title} — Reader</title><style>
*{box-sizing:border-box}html{color-scheme:light dark}body{margin:0;background:#faf8f3;color:#292925;font:20px/1.65 Georgia,serif}
main{max-width:760px;margin:auto;padding:44px 32px 80px}.source{font:14px/1.5 system-ui,sans-serif;color:#526559}
h1{font-size:2em;line-height:1.2;margin:20px 0}h2,h3,h4{line-height:1.3}a{color:#286949;text-underline-offset:3px}
.meta{font:15px/1.5 system-ui,sans-serif;color:#686b63;margin-bottom:36px}article{overflow-wrap:anywhere}
img{max-width:100%;height:auto}figure{margin:1.5em 0}figcaption{font-size:.8em;color:#686b63}blockquote{margin-left:0;padding-left:1.2em;border-left:3px solid #879b87}
pre{padding:16px;background:#eaeae4;overflow:auto;font-size:.8em}code{font-family:monospace}table{display:block;overflow:auto;border-collapse:collapse;max-width:100%}
th,td{padding:8px 12px;border:1px solid #b9bdb4}hr{border:0;border-top:1px solid #b9bdb4;margin:2em 0}
@media(prefers-color-scheme:dark){body{background:#232621;color:#e4e6dc}a{color:#a7d7b0}.source,.meta,figcaption{color:#b6bdaf}pre{background:#32372f}}
@media(max-width:520px){body{font-size:18px}main{padding:24px 20px 60px}h1{font-size:1.7em}}
</style></head><body><main><a class="source" href="${escape(source.href)}" rel="noreferrer">${escape(source.hostname)} · View original</a>
<h1>${title}</h1><p class="meta">${meta}</p><article>${container.innerHTML}</article></main></body></html>`;
    return JSON.stringify({url: source.href, title: article.title || document.title, html});
})()
