#pragma once
#include <Button.h>
#include <View.h>
#include <string>
#include <vector>
#include <memory>

class BBitmap;

namespace summit {
enum class Icon { Back, Forward, Reload, Stop, Plus, Bookmark, BookmarkFilled, Downloads, Home, More };
class ToolButton : public BButton {
public:
    ToolButton(const char* name, const char* tooltip, Icon icon, uint32 message);
    void Draw(BRect update) override;
    void SetIcon(Icon icon) { fIcon = icon; Invalidate(); }
private:
    Icon fIcon;
};
#if SUMMIT_MODERN_WEBKIT
class ExtensionActionButton : public BButton {
public:
    explicit ExtensionActionButton(const char* identifier);
    ~ExtensionActionButton() override;
    void SetAction(const BMessage&, uint64 snapshot);
    void Draw(BRect update) override;
private:
    std::unique_ptr<BBitmap> fBitmap;
    std::string fBadge;
    rgb_color fBadgeBackground { 217, 0, 0, 255 };
    rgb_color fBadgeTextColor { 255, 255, 255, 255 };
};
#endif
struct TabLabel { int64 id; std::string title; bool loading; const BBitmap* icon = nullptr; };
// Draws a site icon, or a neutral globe when there is none.
void DrawSiteIcon(BView* view, const BBitmap* icon, BPoint leftTop);
class TabStrip : public BView {
public:
    TabStrip();
    void SetTabs(std::vector<TabLabel> tabs, int64 selected);
    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void FrameResized(float width, float height) override;
private:
    BRect TabRect(size_t index) const;
    size_t FirstVisible() const;
    size_t VisibleCount() const;
    std::vector<TabLabel> fTabs;
    int64 fSelected = 0;
};
struct BookmarkButton { std::string url, title; const BBitmap* icon = nullptr; };
// Safari-style favourites under the toolbar. Clicking opens a bookmark in the
// current tab; a middle click or Command-click opens it in a new tab; the
// secondary button offers the rest. Messages go to the window.
class BookmarksBar : public BView {
public:
    BookmarksBar();
    void SetBookmarks(std::vector<BookmarkButton> bookmarks);
    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
    void FrameResized(float width, float height) override;
private:
    void LayoutItems();
    int32 ItemAt(BPoint where) const;
    void ShowOverflow();
    void ShowContextMenu(int32 index, BPoint where);
    void Open(int32 index, bool newTab);
    std::vector<BookmarkButton> fBookmarks;
    std::vector<BRect> fRects;
    size_t fVisible = 0;
    BRect fOverflow;
    int32 fHover = -1, fPressed = -1;
    uint32 fPressedButtons = 0;
};
class ProgressLine : public BView {
public:
    ProgressLine();
    void SetProgress(float progress);
    void Draw(BRect update) override;
private:
    float fProgress = 0;
};
}
