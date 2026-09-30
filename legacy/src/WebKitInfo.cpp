/*
 * Copyright 2026 Summit contributors. Distributed under the terms of the MIT License.
 */
#include "WebKitInfo.h"

#include <WebKit/WebKitInfo.h>

#include <cstdlib>
#include <cstring>

namespace {
int
Part(int index)
{
	// BWebKitVersion() is "major.minor.tiny".
	const char* version = BWebKitVersion();
	for (int i = 0; i < index && version != nullptr; i++) {
		version = strchr(version, '.');
		if (version != nullptr)
			version++;
	}
	return version != nullptr ? atoi(version) : 0;
}
}


/*static*/ BString
WebKitInfo::HaikuWebKitVersion()
{
	return BString("Summit ") << BWebKitPortVersion();
}


/*static*/ BString
WebKitInfo::WebKitVersion()
{
	return BWebKitVersion();
}


/*static*/ int
WebKitInfo::WebKitMajorVersion()
{
	return Part(0);
}


/*static*/ int
WebKitInfo::WebKitMinorVersion()
{
	return Part(1);
}


/*static*/ int
WebKitInfo::WebKitTinyVersion()
{
	return Part(2);
}
