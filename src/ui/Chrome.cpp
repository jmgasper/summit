#include "Chrome.h"
#include "Messages.h"
#include <Window.h>
#include <Message.h>
#if SUMMIT_MODERN_WEBKIT
#include "ExtensionInstaller.h"
#include <Bitmap.h>
#include <cstring>
#endif
#include <algorithm>
#include <cmath>

namespace summit {
ToolButton::ToolButton(const char* name, const char* tooltip, Icon icon, uint32 message)
    : BButton(name, "", new BMessage(message)), fIcon(icon)
{
    SetExplicitMinSize(BSize(34, 32));
    SetExplicitMaxSize(BSize(34, 32));
    SetToolTip(tooltip);
}
void ToolButton::Draw(BRect)
{
    SetHighColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    FillRect(Bounds());
    if (Value() || IsFocus()) {
        SetHighColor(Value() ? rgb_color{212, 225, 222, 255} : rgb_color{225, 232, 230, 255});
        FillRoundRect(Bounds().InsetByCopy(2, 2), 5, 5);
    }
    SetHighColor(IsEnabled() ? rgb_color{49, 69, 66, 255} : rgb_color{163, 170, 168, 255});
    SetPenSize(1.6f);
    BPoint c(Bounds().Width() / 2, Bounds().Height() / 2);
    auto line = [&](float x1, float y1, float x2, float y2) {
        StrokeLine(c + BPoint(x1, y1), c + BPoint(x2, y2));
    };
    switch (fIcon) {
        case Icon::More:
            for (float x : {-6.0f, 0.0f, 6.0f}) FillEllipse(c + BPoint(x, 0), 1.3f, 1.3f);
            break;
        case Icon::Back: line(3, -6, -3, 0); line(-3, 0, 3, 6); break;
        case Icon::Forward: line(-3, -6, 3, 0); line(3, 0, -3, 6); break;
        case Icon::Plus: line(-6, 0, 6, 0); line(0, -6, 0, 6); break;
        case Icon::Stop: line(-5, -5, 5, 5); line(5, -5, -5, 5); break;
        case Icon::Reload:
            StrokeArc(BRect(c.x - 6, c.y - 6, c.x + 6, c.y + 6), 40, 290);
            line(6, -7, 6, -1); line(6, -1, 1, -2); break;
        case Icon::Sidebar:
            StrokeRoundRect(BRect(c.x - 9, c.y - 6, c.x + 9, c.y + 6), 2, 2);
            line(-3, -6, -3, 6); break;
        case Icon::Bookmark: {
            BPoint points[10];
            for (int i = 0; i < 10; ++i) {
                const float angle = -1.5707963f + i * 0.6283185f;
                const float radius = i % 2 ? 3.5f : 8.0f;
                points[i] = c + BPoint(std::cos(angle) * radius, std::sin(angle) * radius);
            }
            StrokePolygon(points, 10); break;
        }
        case Icon::Downloads:
            line(0, -8, 0, 3); line(-4, -1, 0, 3); line(0, 3, 4, -1);
            line(-7, 3, -7, 7); line(-7, 7, 7, 7); line(7, 7, 7, 3); break;
        case Icon::Home:
            line(-8, 1, 0, -7); line(0, -7, 8, 1);
            line(-5, -1, -5, 7); line(-5, 7, 5, 7); line(5, 7, 5, -1); break;
    }
    SetPenSize(1);
}

#if SUMMIT_MODERN_WEBKIT
ExtensionActionButton::ExtensionActionButton(const char* identifier)
    : BButton((std::string("extension-action-") + identifier).c_str(), "", nullptr)
{
    SetExplicitMinSize(BSize(34, 32));
    SetExplicitMaxSize(BSize(34, 32));
}
ExtensionActionButton::~ExtensionActionButton() = default;

void ExtensionActionButton::SetAction(const BMessage& action, uint64 snapshot)
{
    const char* title = "";
    action.FindString("title", &title);
    if (!*title) action.FindString("name", &title);
    auto label = ExtensionDisplayText(title);
    SetLabel(label.c_str());
    const char* badge = "";
    action.FindString("badge", &badge);
    fBadge = ExtensionDisplayText(badge);
    SetToolTip((label + (fBadge.empty() ? "" : " — " + fBadge)).c_str());
    auto* message = new BMessage(action);
    message->what = kActivateExtensionAction;
    message->AddUInt64("snapshot", snapshot);
    SetMessage(message);
    fBitmap.reset();
    const void* pixels = nullptr;
    ssize_t length = 0;
    if (action.FindData("icon_bgra", B_RAW_TYPE, &pixels, &length) == B_OK && length == 16 * 16 * 4) {
        auto bitmap = std::make_unique<BBitmap>(BRect(0, 0, 15, 15), B_RGBA32);
        if (bitmap->InitCheck() == B_OK) {
            for (int row = 0; row < 16; ++row)
                std::memcpy(static_cast<uint8*>(bitmap->Bits()) + row * bitmap->BytesPerRow(),
                    static_cast<const uint8*>(pixels) + row * 64, 64);
            fBitmap = std::move(bitmap);
        }
    }
    SetEnabled(action.GetBool("enabled", false));
    Invalidate();
}

void ExtensionActionButton::Draw(BRect)
{
    SetDrawingMode(B_OP_COPY);
    SetHighColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    FillRect(Bounds());
    if (Value() || IsFocus()) {
        SetHighColor(Value() ? rgb_color{212, 225, 222, 255} : rgb_color{225, 232, 230, 255});
        FillRoundRect(Bounds().InsetByCopy(2, 2), 5, 5);
    }
    const BPoint origin((Bounds().Width() - 15) / 2, (Bounds().Height() - 15) / 2);
    if (fBitmap) {
        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        DrawBitmap(fBitmap.get(), origin);
        SetDrawingMode(B_OP_COPY);
    } else {
        SetHighColor(IsEnabled() ? rgb_color{49, 99, 84, 255} : rgb_color{163, 170, 168, 255});
        SetPenSize(1.5f);
        StrokeRoundRect(BRect(origin, origin + BPoint(15, 15)), 3, 3);
        StrokeLine(origin + BPoint(4, 7.5f), origin + BPoint(11, 7.5f));
        StrokeLine(origin + BPoint(7.5f, 4), origin + BPoint(7.5f, 11));
        SetPenSize(1);
    }
    if (!IsEnabled() && fBitmap) {
        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
        auto color = ui_color(B_PANEL_BACKGROUND_COLOR); color.alpha = 160;
        SetHighColor(color);
        FillRect(BRect(origin, origin + BPoint(15, 15)));
        SetDrawingMode(B_OP_COPY);
    }
    if (!fBadge.empty()) {
        BFont font(be_bold_font); font.SetSize(9);
        SetFont(&font);
        BString badge(fBadge.c_str());
        TruncateString(&badge, B_TRUNCATE_END, 21);
        float width = std::max(12.0f, StringWidth(badge.String()) + 4);
        BRect rect(Bounds().right - width, 1, Bounds().right, 13);
        SetHighColor(IsEnabled() ? rgb_color{38, 105, 83, 255} : rgb_color{130, 140, 136, 255});
        FillRoundRect(rect, 3, 3);
        SetHighColor(255, 255, 255);
        SetDrawingMode(B_OP_OVER);
        DrawString(badge.String(), BPoint(rect.left + 2, 10));
        SetDrawingMode(B_OP_COPY);
        SetFont(be_plain_font);
    }
}
#endif

TabStrip::TabStrip() : BView("tabs", B_WILL_DRAW | B_FRAME_EVENTS)
{
    SetViewColor(B_TRANSPARENT_COLOR);
    SetExplicitMinSize(BSize(200, 36));
    SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 36));
}
void TabStrip::SetTabs(std::vector<TabLabel> tabs, int64 selected)
{
    fTabs = std::move(tabs); fSelected = selected; Invalidate();
}
size_t TabStrip::VisibleCount() const
{
    return std::min(fTabs.size(), std::max(size_t(1), size_t(std::max(100.0f, Bounds().Width() - 60) / 120)));
}
size_t TabStrip::FirstVisible() const
{
    size_t selected = 0;
    for (size_t i = 0; i < fTabs.size(); ++i) if (fTabs[i].id == fSelected) selected = i;
    const size_t count = VisibleCount();
    return selected >= count ? selected - count + 1 : 0;
}
BRect TabStrip::TabRect(size_t index) const
{
    const size_t count = VisibleCount();
    float width = std::min(250.0f, (Bounds().Width() - 60) / std::max(size_t(1), count));
    const float left = 8 + (index - FirstVisible()) * width;
    return BRect(left, 3, left + width - 3, Bounds().bottom - 3);
}
void TabStrip::Draw(BRect)
{
    SetHighColor(230, 233, 232); FillRect(Bounds());
    const size_t first = FirstVisible(), last = first + VisibleCount();
    for (size_t i = first; i < last; ++i) {
        BRect rect = TabRect(i);
        const bool selected = fTabs[i].id == fSelected;
        SetHighColor(selected ? rgb_color{252, 253, 252, 255} : rgb_color{230, 233, 232, 255});
        FillRoundRect(rect, 5, 5);
        SetHighColor(selected ? rgb_color{30, 82, 72, 255} : rgb_color{78, 86, 83, 255});
        BString text(fTabs[i].title.empty() ? "New Tab" : fTabs[i].title.c_str());
        TruncateString(&text, B_TRUNCATE_END, rect.Width() - 51);
        if (fTabs[i].loading) FillEllipse(BRect(rect.left + 9, 15, rect.left + 13, 19));
        DrawString(text, BPoint(rect.left + 21, 22));
        StrokeLine(BPoint(rect.right - 16, 14), BPoint(rect.right - 10, 20));
        StrokeLine(BPoint(rect.right - 10, 14), BPoint(rect.right - 16, 20));
    }
    SetHighColor(63, 83, 76);
    const float x = Bounds().right - 24;
    StrokeLine(BPoint(x - 5, 18), BPoint(x + 5, 18));
    StrokeLine(BPoint(x, 13), BPoint(x, 23));
}
void TabStrip::MouseDown(BPoint point)
{
    if (point.x > Bounds().right - 44) { Window()->PostMessage(kNewTab); return; }
    uint32 buttons = 0;
    Window()->CurrentMessage()->FindInt32("buttons", reinterpret_cast<int32*>(&buttons));
    for (size_t i = FirstVisible(); i < FirstVisible() + VisibleCount(); ++i) {
        const auto rect = TabRect(i);
        if (!rect.Contains(point)) continue;
        const bool close = point.x > rect.right - 24 || buttons & B_TERTIARY_MOUSE_BUTTON;
        BMessage message(close ? kCloseTab : kSelectTab);
        message.AddInt64("id", fTabs[i].id);
        Window()->PostMessage(&message);
        return;
    }
}
void TabStrip::FrameResized(float, float) { Invalidate(); }
ProgressLine::ProgressLine() : BView("progress", B_WILL_DRAW)
{
    SetViewColor(B_TRANSPARENT_COLOR);
    SetExplicitMinSize(BSize(0, 2));
    SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 2));
}
void ProgressLine::SetProgress(float value) { fProgress = std::clamp(value, 0.0f, 1.0f); Invalidate(); }
void ProgressLine::Draw(BRect)
{
    SetHighColor(214, 221, 217); FillRect(Bounds());
    if (fProgress > 0 && fProgress < 1) {
        SetHighColor(44, 125, 104);
        BRect progress = Bounds(); progress.right *= fProgress; FillRect(progress);
    }
}
}
