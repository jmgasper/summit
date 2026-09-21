/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

// This suite exercises package snapshots, staging directories and install
// state without the browser runtime. It does not call
// prepareWebExtensionPackageHaiku(), but that function sits in the snapshot
// translation unit and its unwind information keeps it in the link, so its
// archive entry points have to resolve. The real implementations need libzip
// and the CRX verifier, which this suite deliberately leaves out; these
// definitions stand in for them and abort if a test ever reaches one.

#include "config.h"
#include "WebExtensionArchiveHaiku.h"
#include <cstdio>
#include <cstdlib>

namespace WebKit {

[[noreturn]] static void unavailable(const char* name)
{
    std::fprintf(stderr, "%s is not available in the package snapshot test\n", name);
    std::abort();
}

WebExtensionTemporaryDirectoryHaiku::WebExtensionTemporaryDirectoryHaiku(WebExtensionTemporaryDirectoryHaiku&&)
{
    unavailable("Archive extraction");
}

WebExtensionTemporaryDirectoryHaiku::~WebExtensionTemporaryDirectoryHaiku()
{
    unavailable("Archive extraction");
}

std::expected<WebExtensionTemporaryDirectoryHaiku, WebExtensionArchiveErrorHaiku> extractWebExtensionArchiveHaiku(const String&, const WebExtensionArchiveLimitsHaiku&, const String&)
{
    unavailable("Archive extraction");
}

} // namespace WebKit
