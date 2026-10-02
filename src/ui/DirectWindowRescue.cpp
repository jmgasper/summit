// BDirectWindow::_DisposeData() waits until fConnectionEnable is false, which
// the window's direct daemon thread sets when app_server stops the connection.
// When app_server killed that thread (a DirectConnected() over its time limit)
// the flag stays set and closing the window never finishes. The flag is
// private: this file alone sees the class with its private members public.
#define private public
#include <DirectWindow.h>
#undef private

#include "DirectWindowRescue.h"

namespace summit {
void ReleaseDeadDirectConnection(BDirectWindow& window)
{
    window.fConnectionEnable = false;
}
}
