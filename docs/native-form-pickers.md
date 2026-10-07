# Native form pickers

Issue [#35](https://github.com/jmgasper/summit/issues/35) connects modern WebKit's
color input to Haiku's `BColorControl`. Clicking a color input opens **Choose
color**, initialized with its current RGB value. **Done** updates the HTML
value and sends the normal input/change events; **Cancel** preserves the
value. Up to ten author-provided datalist colors appear as native swatches.
Changes made by page scripts update the open control without generating
user-edit events. Navigating, switching away from the tab, or closing it
dismisses the picker.

The WebProcess retains HTML validation and activation checks.
`WebColorPickerHaiku` uses the existing color chooser IPC and schedules native
results on the WebKit main loop. The window looper owns its controls and native
data; it never dereferences a page or chooser client. Cancellation clears the
main-loop callback before asynchronously dismissing the native window.

The standard RGB color input is enabled; experimental alpha and wide-gamut
color input enhancements remain disabled because `BColorControl` edits RGB.
The datalist DOM is enabled for the chooser's suggestions; a general text-input
datalist dropdown still needs a native implementation.

`tools/bench/pages/color-input.html` and `tests/ModernColorInputTests.cpp`
exercise the actual renderer, native controls, script updates, DOM events,
suggestions, and teardown. Compile the native test with
`-std=c++23 -Isrc -Ivendor -lbe`, start an owned browser with a disposable profile
at the HTTP fixture, and run:

```sh
test-color TEAM EXECUTABLE FIXTURE_URL SCREENSHOT_PPM
```

X399 validation on 7 October 2026: bundle `bundle-9e85uc_r`, engine patch
`bca6ac58405fc96958b51fcf00ba7a2b4b41958eeda7b15a7b9c44c0bc9fac8f`,
65/65 native checks pass. Owned team 54995 and its helpers exited normally;
there were no new crash reports or debugger events. The native screenshot
was inspected. Local evidence: `.vm/issue35-native-results.json` and
`.vm/issue35-picker.png`.
