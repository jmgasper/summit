#pragma once
#include <Messenger.h>
#include <SupportDefs.h>

namespace summit {
// Finds where this computer is, for pages the user allowed to know it. Haiku
// has no location service of its own (no GPS or cell modem support, nothing
// like GeoClue), so the position comes from BeaconDB (https://beacondb.net),
// the open successor of Mozilla Location Service: the Wi-Fi networks in range
// (their hardware addresses) when there are at least two, and the network
// address the request comes from otherwise. Runs on a thread of its own and
// sends `what` to reply with "latitude", "longitude", "accuracy" (metres) and
// "timestamp" (seconds since the epoch), or with "error".
void LookUpLocation(BMessenger reply, uint32 what);
}
