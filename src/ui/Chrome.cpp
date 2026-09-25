#include "Chrome.h"
#include "Messages.h"
#include <Window.h>
#include <Message.h>
#include <Bitmap.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#if SUMMIT_MODERN_WEBKIT
#include "ExtensionInstaller.h"
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
        case Icon::Bookmark: case Icon::BookmarkFilled: {
            BPoint points[10];
            for (int i = 0; i < 10; ++i) {
                const float angle = -1.5707963f + i * 0.6283185f;
                const float radius = i % 2 ? 3.5f : 8.0f;
                points[i] = c + BPoint(std::cos(angle) * radius, std::sin(angle) * radius);
            }
            if (fIcon == Icon::BookmarkFilled && IsEnabled()) {
                // The current page is bookmarked.
                SetHighColor(44, 125, 104);
                FillPolygon(points, 10);
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
    const auto unpack = [](uint32 color) {
        return rgb_color { uint8(color >> 24), uint8(color >> 16), uint8(color >> 8), uint8(color) };
    };
    fBadgeBackground = unpack(action.GetUInt32("badge_background_rgba", 0xD90000FF));
    fBadgeTextColor = unpack(action.GetUInt32("badge_text_rgba", 0xFFFFFFFF));
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
        auto background = fBadgeBackground;
        auto foreground = fBadgeTextColor;
        if (!IsEnabled()) {
            background.alpha = uint8(background.alpha * 0.55f);
            foreground.alpha = uint8(foreground.alpha * 0.55f);
        }
        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        SetHighColor(background);
        FillRoundRect(rect, 3, 3);
        SetHighColor(foreground);
        DrawString(badge.String(), BPoint(rect.left + 2, 10));
        SetDrawingMode(B_OP_COPY);
        SetFont(be_plain_font);
    }
}
#endif

void DrawSiteIcon(BView* view, const BBitmap* icon, BPoint leftTop)
{
    if (icon) {
        view->PushState();
        view->SetDrawingMode(B_OP_ALPHA);
        view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        view->DrawBitmap(icon, BRect(leftTop, leftTop + BPoint(15, 15)));
        view->PopState();
        return;
    }
    view->PushState();
    view->SetHighColor(128, 142, 135);
    view->SetPenSize(1.2f);
    const BPoint centre = leftTop + BPoint(7.5f, 7.5f);
    view->StrokeEllipse(centre, 6.5f, 6.5f);
    view->StrokeEllipse(centre, 2.8f, 6.5f);
    view->StrokeLine(centre + BPoint(-6.5f, 0), centre + BPoint(6.5f, 0));
    view->PopState();
}

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
        TruncateString(&text, B_TRUNCATE_END, rect.Width() - 61);
        const BPoint icon(rect.left + 9, rect.top + std::floor((rect.Height() - 15) / 2));
        if (fTabs[i].loading && !fTabs[i].icon) FillEllipse(icon + BPoint(7.5f, 7.5f), 2.5f, 2.5f);
        else DrawSiteIcon(this, fTabs[i].icon, icon);
        if (fTabs[i].loading && fTabs[i].icon) {
            // A loading page keeps its icon, with a dot on its corner.
            SetHighColor(44, 125, 104);
            FillEllipse(icon + BPoint(15, 15), 2.5f, 2.5f);
            SetHighColor(selected ? rgb_color{30, 82, 72, 255} : rgb_color{78, 86, 83, 255});
        }
        DrawString(text, BPoint(rect.left + 31, 22));
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
static constexpr int32 kOverflowItem = -2;
BookmarksBar::BookmarksBar() : BView("bookmarks-bar", B_WILL_DRAW | B_FRAME_EVENTS)
{
    SetViewColor(B_TRANSPARENT_COLOR);
    SetExplicitMinSize(BSize(200, 27));
    SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 27));
}
void BookmarksBar::SetBookmarks(std::vector<BookmarkButton> bookmarks)
{
    fBookmarks = std::move(bookmarks);
    fHover = fPressed = -1;
    LayoutItems();
    Invalidate();
}
void BookmarksBar::LayoutItems()
{
    fRects.clear();
    const float height = Bounds().Height();
    float x = 6;
    for (const auto& bookmark : fBookmarks) {
        float width = 8 + 16 + 8;
        if (!bookmark.title.empty()) width += 5 + std::min(150.0f, std::ceil(StringWidth(bookmark.title.c_str())));
        fRects.push_back(BRect(x, 2, x + width - 1, height - 3));
        x += width + 2;
    }
    fVisible = fRects.size();
    fOverflow = BRect();
    if (!fRects.empty() && fRects.back().right > Bounds().right - 6) {
        // Keep room for a » button that lists the rest.
        fOverflow = BRect(Bounds().right - 30, 2, Bounds().right - 6, height - 3);
        fVisible = 0;
        while (fVisible < fRects.size() && fRects[fVisible].right < fOverflow.left - 4) ++fVisible;
    }
}
void BookmarksBar::FrameResized(float, float) { LayoutItems(); Invalidate(); }
int32 BookmarksBar::ItemAt(BPoint where) const
{
    if (fOverflow.IsValid() && fOverflow.Contains(where)) return kOverflowItem;
    for (size_t i = 0; i < fVisible; ++i) if (fRects[i].Contains(where)) return int32(i);
    return -1;
}
void BookmarksBar::Draw(BRect)
{
    const BRect bounds = Bounds();
    SetHighColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    FillRect(bounds);
    SetHighColor(221, 227, 224);
    StrokeLine(BPoint(bounds.left, bounds.bottom), BPoint(bounds.right, bounds.bottom));
    font_height metrics;
    GetFontHeight(&metrics);
    const float baseline = std::floor((bounds.Height() + metrics.ascent - metrics.descent) / 2);
    if (fBookmarks.empty()) {
        SetHighColor(135, 147, 141);
        DrawString("Add favourite pages here with Bookmarks › Add to Bookmarks Bar", BPoint(12, baseline));
        return;
    }
    for (size_t i = 0; i < fVisible; ++i) {
        const BRect rect = fRects[i];
        if (int32(i) == fPressed || int32(i) == fHover) {
            SetHighColor(int32(i) == fPressed ? rgb_color{212, 225, 222, 255} : rgb_color{225, 232, 230, 255});
            FillRoundRect(rect, 5, 5);
        }
        DrawSiteIcon(this, fBookmarks[i].icon, BPoint(rect.left + 8, std::floor(rect.top + (rect.Height() - 15) / 2)));
        if (fBookmarks[i].title.empty()) continue;
        BString title(fBookmarks[i].title.c_str());
        TruncateString(&title, B_TRUNCATE_END, 150);
        SetHighColor(49, 69, 66);
        DrawString(title.String(), BPoint(rect.left + 29, baseline));
    }
    if (fOverflow.IsValid()) {
        if (fHover == kOverflowItem) {
            SetHighColor(225, 232, 230);
            FillRoundRect(fOverflow, 5, 5);
        }
        SetHighColor(49, 69, 66);
        SetPenSize(1.4f);
        const BPoint c(std::floor((fOverflow.left + fOverflow.right) / 2), std::floor((fOverflow.top + fOverflow.bottom) / 2));
        for (float dx : {-3.0f, 2.0f}) {
            StrokeLine(c + BPoint(dx - 2, -4), c + BPoint(dx + 2, 0));
            StrokeLine(c + BPoint(dx + 2, 0), c + BPoint(dx - 2, 4));
        }
        SetPenSize(1);
    }
}
void BookmarksBar::MouseDown(BPoint where)
{
    uint32 buttons = 0;
    if (auto* message = Window()->CurrentMessage())
        message->FindInt32("buttons", reinterpret_cast<int32*>(&buttons));
    const int32 index = ItemAt(where);
    if (index == kOverflowItem) { ShowOverflow(); return; }
    if (index < 0) return;
    if (buttons & B_SECONDARY_MOUSE_BUTTON) { ShowContextMenu(index, where); return; }
    fPressed = index;
    fPressedButtons = buttons;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    Invalidate();
}
void BookmarksBar::MouseUp(BPoint where)
{
    if (fPressed >= 0 && ItemAt(where) == fPressed)
        Open(fPressed, (fPressedButtons & B_TERTIARY_MOUSE_BUTTON) || (modifiers() & B_COMMAND_KEY));
    fPressed = -1;
    Invalidate();
}
void BookmarksBar::MouseMoved(BPoint where, uint32 transit, const BMessage*)
{
    const int32 hover = transit == B_EXITED_VIEW || transit == B_OUTSIDE_VIEW ? -1 : ItemAt(where);
    if (hover == fHover) return;
    fHover = hover;
    if (hover >= 0) SetToolTip(fBookmarks[hover].url.c_str());
    else SetToolTip(hover == kOverflowItem ? "More bookmarks" : static_cast<const char*>(nullptr));
    Invalidate();
}
void BookmarksBar::Open(int32 index, bool newTab)
{
    if (index < 0 || size_t(index) >= fBookmarks.size()) return;
    BMessage open(kOpenBookmark);
    open.AddString("url", fBookmarks[index].url.c_str());
    open.AddBool("new_tab", newTab);
    Window()->PostMessage(&open);
}
static BMessage* BookmarkMessage(uint32 what, const std::string& url, bool newTab = false)
{
    auto* message = new BMessage(what);
    message->AddString("url", url.c_str());
    if (newTab) message->AddBool("new_tab", true);
    return message;
}
void BookmarksBar::ShowOverflow()
{
    auto* menu = new BPopUpMenu("more-bookmarks", false, false);
    for (size_t i = fVisible; i < fBookmarks.size(); ++i) {
        const auto& title = fBookmarks[i].title.empty() ? fBookmarks[i].url : fBookmarks[i].title;
        menu->AddItem(new BMenuItem(title.c_str(), BookmarkMessage(kOpenBookmark, fBookmarks[i].url)));
    }
    menu->SetTargetForItems(Window());
    menu->SetAsyncAutoDestruct(true);
    menu->Go(ConvertToScreen(BPoint(fOverflow.left, fOverflow.bottom + 1)), true, false,
        ConvertToScreen(fOverflow), true);
}
void BookmarksBar::ShowContextMenu(int32 index, BPoint where)
{
    const auto& url = fBookmarks[index].url;
    auto* menu = new BPopUpMenu("bookmark", false, false);
    menu->AddItem(new BMenuItem("Open", BookmarkMessage(kOpenBookmark, url)));
    menu->AddItem(new BMenuItem("Open in New Tab", BookmarkMessage(kOpenBookmark, url, true)));
    menu->AddSeparatorItem();
    menu->AddItem(new BMenuItem("Remove from Bookmarks Bar", BookmarkMessage(kRemoveFromBookmarksBar, url)));
    menu->AddItem(new BMenuItem("Delete Bookmark", BookmarkMessage(kRemoveBookmark, url)));
    menu->SetTargetForItems(Window());
    menu->SetAsyncAutoDestruct(true);
    menu->Go(ConvertToScreen(where), true, false, true);
}

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
