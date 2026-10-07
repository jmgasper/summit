#include "CertificateInfoWindow.h"
#if SUMMIT_MODERN_WEBKIT
#include "ExtensionInstaller.h"
#include "Messages.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextView.h>
#include <cmath>
#include <ctime>

namespace summit {
namespace {
std::string Date(double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 253402300799.0) return "Unknown";
    const time_t time = static_cast<time_t>(seconds);
    struct tm utc {};
    char text[80];
    if (!gmtime_r(&time, &utc) || !std::strftime(text, sizeof(text), "%d %B %Y, %H:%M:%S UTC", &utc)) return "Unknown";
    return text;
}
}
CertificateInfoWindow::CertificateInfoWindow(const BMessage& certificate, BRect owner)
    : BWindow(BRect(0, 0, 570, 410), "Connection security — Summit", B_TITLED_WINDOW,
        B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE | B_NOT_ZOOMABLE)
{
    const bool verified = certificate.GetBool("verified", false);
    const bool exception = certificate.GetBool("exception", false);
    const bool mixed = certificate.GetBool("mixedContent", false);
    const char* summary = !verified ? "Certificate not verified" : mixed ? "This page includes insecure content" : "Connection is encrypted and verified";
    auto* heading = new BStringView("certificate-heading", summary);
    heading->SetFont(be_bold_font);
    auto value = [&](const char* key) { return ExtensionDisplayText(certificate.GetString(key, "")); };
    std::string text = "Page: " + value("url") + "\n\n";
    if (exception) text += "This certificate was accepted using a saved exception for this host.\n\n";
    else if (!verified) text += "Summit could not verify this certificate.\n\n";
    if (!verified && !value("problem").empty()) text += "Certificate warning: " + value("problem") + "\n\n";
    if (mixed) text += "Some content on this page was loaded without a secure connection.\n\n";
    text += "Issued to: " + value("subject") + "\n\nIssued by: " + value("issuer") + "\n\n";
    if (!value("names").empty()) text += "Valid for: " + value("names") + "\n\n";
    text += "Valid from: " + Date(certificate.GetDouble("validFrom", 0)) + "\n";
    text += "Valid until: " + Date(certificate.GetDouble("validUntil", 0)) + "\n\nSHA-256 fingerprint:\n";
    const auto fingerprint = value("sha256");
    for (size_t i = 0; i < fingerprint.size(); ++i) {
        if (i && i % 2 == 0) text += i % 32 ? ":" : "\n";
        const char c = fingerprint[i];
        text += c >= 'a' && c <= 'f' ? c - 'a' + 'A' : c;
    }
    auto* details = new BTextView("certificate-details");
    details->MakeEditable(false);
    details->MakeSelectable(true);
    details->SetWordWrap(true);
    details->SetInsets(12, 12, 12, 12);
    details->SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
    details->SetLowUIColor(B_DOCUMENT_BACKGROUND_COLOR);
    auto color = ui_color(B_DOCUMENT_TEXT_COLOR);
    details->SetFontAndColor(be_plain_font, B_FONT_ALL, &color);
    details->SetText(text.c_str());
    auto* scroll = new BScrollView("certificate-scroll", details, 0, false, true);
    scroll->SetExplicitMinSize(BSize(430, 260));
    auto* close = new BButton("certificate-close", "Close", new BMessage(B_QUIT_REQUESTED));
    BLayoutBuilder::Group<>(this, B_VERTICAL, 12).SetInsets(16)
        .Add(heading).Add(scroll).AddGroup(B_HORIZONTAL).AddGlue().Add(close).End();
    SetDefaultButton(close);
    CenterIn(owner);
}
void CertificateInfoWindow::MessageReceived(BMessage* message)
{
    if (message->what == kShowCertificate) { Activate(); return; }
    BWindow::MessageReceived(message);
}
}
#endif
