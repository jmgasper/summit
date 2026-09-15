"use strict";
if (location.pathname.endsWith("/target/test")) {
    const probe = document.createElement("div");
    probe.id = "i18n-probe";
    document.body.append(probe);
    runI18nSuite("content");
}
