#pragma once
class BDirectWindow;

namespace summit {
// For a direct window whose daemon thread app_server killed (it gives
// DirectConnected() half a second): clears the connection flag only that
// thread would have cleared, so ~BDirectWindow does not wait for ever.
void ReleaseDeadDirectConnection(BDirectWindow&);
}
