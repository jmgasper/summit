#pragma once
#include <Button.h>
#include <View.h>
#include <string>
#include <vector>
#include <memory>

class BBitmap;

namespace summit {
enum class Icon { Sidebar, Back, Forward, Reload, Stop, Plus, Bookmark, Downloads, Home, More };
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
struct TabLabel { int64 id; std::string title; bool loading; };
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
class ProgressLine : public BView {
public:
    ProgressLine();
    void SetProgress(float progress);
    void Draw(BRect update) override;
private:
    float fProgress = 0;
};
}
