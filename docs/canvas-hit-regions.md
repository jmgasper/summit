# Canvas hit regions

Summit's Skia renderer supports `addHitRegion()`, `removeHitRegion()` and
`clearHitRegions()` on an HTML canvas's 2D context. Native mouse events expose
the region's ID through the read-only, nullable `MouseEvent.region` property.
This implements the legacy canvas feature checked by HTML5test, following the
[2015 Canvas 2D hit-region model](https://www.w3.org/TR/2015/CR-2dcontext-20150702/#hit-regions).

```js
const context = canvas.getContext("2d");
context.beginPath();
context.roundRect(20, 20, 120, 50, 8);
context.fill();
context.addHitRegion({ id: "open" });

canvas.addEventListener("click", event => {
    if (event.region === "open")
        openPanel();
});
```

`addHitRegion()` snapshots the current path, transform and clipping region.
An explicit `Path2D` can be supplied with `path`, with `fillRule` set to
`"nonzero"` or `"evenodd"`. At least one of `id` and `control` must be supplied;
an empty area or missing identifier/control throws `NotSupportedError`.
Invalid replacements leave the previous region intact.

IDs are case sensitive. Reusing an ID or control replaces its previous region.
New regions take ownership of overlapping areas, so removing one does not
restore the region previously underneath it. `clearRect()` subtracts its
transformed, clipped area; ordinary painting preserves regions. Context reset
and canvas size assignment clear all regions.

Mouse coordinates account for CSS transforms, scaling, borders, padding,
scrolling and `object-fit`. Events still target the canvas, and retain their
region ID if the region is subsequently changed or removed. An event outside
a named region has `region === null`. Synthetic events preserve the value
supplied through `MouseEventInit.region`.

The optional `control` associates an element with the region. Descendants of
the canvas receive bounds through WebKit's existing canvas accessibility
geometry mechanism. This does not change the event target.
Controls are weak references, so a region cannot retain its owning canvas or
DOM subtree through a self, ancestor or mutual reference.

Visual canvas recordings omit hit-region changes: they do not draw pixels,
and replay cannot reconstruct their DOM controls.

## Native verification

X399's `bundle-ralgs2gr` passes all 54 native click sequences (560 DOM checks)
and all 20 lifetime/recording checks. The runs leave no helper processes,
new crash reports or crash-related syslog events. The engine patch is
`303c94b24cd5f1cf92493e00a6c30f0985a7e52d3630c24b14f538719850605f`.
The same bundle passes the platform pages (120 checks), grid/reflow geometry,
WebGL pixels and animation, and canvas path/damage pixel regressions.
Evidence is retained in `.vm/issues43-49/{hit-regions-x399-final,
hit-region-lifetime-x399,hit-region-regressions}/`.

The interaction fixture checks the public API with native mouse-down, mouse-up and click
events, including overlap, curves, clipping, transforms, scrolling, reset,
replacement and synthetic-event behavior. It uses an isolated profile and
quits only its owned browser group.

```sh
SUMMIT_BENCH_HOST=workstation SUMMIT_WS_MULTIPLEX=0 \
  python3 tools/bench/test-hit-regions.py \
  --bundle /absolute/frozen/bundle --output .vm/hit-regions
```

The inspector fixture checks destruction of 40 canvases with self, ancestor,
descendant and mutual control references after garbage collection. It also
checks that an active drawing recording retains drawing commands, omits
hit-region commands and releases its canvas afterward.

```sh
SUMMIT_BENCH_HOST=workstation SUMMIT_WS_MULTIPLEX=0 \
  python3 tools/bench/test-hit-region-lifetime.py \
  --bundle /absolute/frozen/bundle --output .vm/hit-region-lifetime
```

`ENABLE_CANVAS_HIT_REGIONS` defaults on when Skia is enabled and is recorded
in the frozen bundle's configuration manifest.
