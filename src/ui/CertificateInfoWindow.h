#pragma once
#if SUMMIT_MODERN_WEBKIT
#include <Window.h>
namespace summit {
class CertificateInfoWindow final : public BWindow {
public:
    CertificateInfoWindow(const BMessage& certificate, BRect owner);
    void MessageReceived(BMessage*) override;
};
}
#endif
