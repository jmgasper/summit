#!/usr/bin/env python3
"""Sort one thread's flat profile into subsystems. profile-buckets.py FILE THREAD_ID"""
import re, sys, collections
text = open(sys.argv[1], errors='replace').read()
block = re.search(r'profiling results for thread "[^"]*" \(%s\):(.*?)(?=\nprofiling results for thread |\Z)' % sys.argv[2], text, re.S).group(1)
total = int(re.search(r'total ticks:\s+(\d+)', block).group(1))
rules = [
    ('style', r'WebCore::Style::|SelectorChecker|SelectorDataList|RuleSet|CSSSelector|ComputedStyle|RenderStyle|CSSPropertyParser|CSSParser|StyleRule|MatchedDeclarations|Style::Builder'),
    ('layout', r'WebCore::Layout::|RenderBlock|RenderFlex|RenderGrid|RenderBox|RenderInline|RenderText|RenderTable|LayoutIntegration|InlineFormatting|RenderElement::layout|RenderLayer|RenderView|RenderObject|LayoutUnit|RenderFragmented|FloatingContext|RenderReplaced|RenderImage'),
    ('text/fonts', r'hb_|FontCascade|ComplexText|WidthIterator|Glyph|Font::|FT_|TextRun|TextBreak|ubrk|icu'),
    ('paint/display list', r'Paint|paint|DisplayList|GraphicsContext|Sk[A-Z]|sk_'),
    ('js', r'JSC::|llint|op_|Interpreter|JIT|DFG|B3|Wasm|vm_entry|js_'),
    ('dom/bindings', r'WebCore::JS|WebCore::Element|WebCore::Node|WebCore::Document|ContainerNode|Attribute|QualifiedName|WebCore::HTML|EventTarget|WebCore::Event'),
    ('parsing', r'HTMLTokenizer|HTMLTreeBuilder|HTMLDocumentParser|HTMLConstructionSite|CSSTokenizer|HTMLEntity|HTMLPreload'),
    ('alloc', r'fastMalloc|fastFree|mi_|malloc|free|omalloc|ofree|memset|memcpy|memmove|operator new|operator delete'),
    ('loader/net/ipc', r'IPC::|Resource|Loader|Cached|Network|Curl|WebKit::'),
    ('wtf/strings', r'WTF::|StringImpl|AtomString'),
]
buckets = collections.Counter()
for hits, us, pct, img, func in re.findall(r'^\s+(\d+)\s+(\d+)\s+([\d.]+)\s+(\d+)\s+(.+)$', block, re.M):
    for name, pattern in rules:
        if re.search(pattern, func):
            buckets[name] += int(hits); break
    else:
        buckets['other'] += int(hits)
for name, hits in buckets.most_common():
    print(f'{name:22} {hits:6} ticks  {100.0 * hits / total:5.1f}%')
print('total', total)
