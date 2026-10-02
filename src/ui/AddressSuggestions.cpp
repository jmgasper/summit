#include "AddressSuggestions.h"
#include "Messages.h"
#include <Bitmap.h>
#include <Screen.h>
#include <View.h>
#include <Window.h>
#include <String.h>
#include <algorithm>
#include <cmath>

namespace summit {
namespace {
constexpr int32 kMaximumRows = 9;

rgb_color Mix(rgb_color a, rgb_color b, float amount)
{
    auto channel = [&](uint8 x, uint8 y) { return static_cast<uint8>(x + (y - x) * amount); };
    return make_color(channel(a.red, b.red), channel(a.green, b.green), channel(a.blue, b.blue));
}

class ListView final : public BView {
public:
    explicit ListView(BMessenger owner)
        : BView("suggestions", B_WILL_DRAW | B_FRAME_EVENTS), fOwner(owner)
    {
        SetViewColor(B_TRANSPARENT_COLOR);
    }

    void SetRows(std::vector<SuggestionRow> rows, int32 selected)
    {
        fRows = std::move(rows);
        fSelected = selected;
        fHovered = -1;
        Invalidate();
    }
    void SetSelected(int32 selected)
    {
        if (selected == fSelected) return;
        fSelected = selected;
        Invalidate();
    }
    float RowHeight()
    {
        font_height height;
        GetFontHeight(&height);
        return std::max(16.0f * IconScale(), std::ceil(height.ascent + height.descent)) + std::ceil(be_plain_font->Size() * 0.7f);
    }
    float IconScale() const { return std::max(1.0f, be_plain_font->Size() / 12.0f); }

    void Draw(BRect) override
    {
        const rgb_color background = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
        const rgb_color text = ui_color(B_DOCUMENT_TEXT_COLOR);
        const rgb_color selectedBackground = ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);
        const rgb_color selectedText = ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR);
        const rgb_color link = ui_color(B_LINK_TEXT_COLOR);
        const float rowHeight = RowHeight();
        const float padding = std::ceil(be_plain_font->Size() * 0.6f);
        const float iconSize = std::round(16 * IconScale());
        font_height height;
        GetFontHeight(&height);
        SetHighColor(background);
        FillRect(Bounds());
        SetDrawingMode(B_OP_ALPHA);
        for (int32 i = 0; i < static_cast<int32>(fRows.size()); ++i) {
            const auto& row = fRows[i];
            BRect frame(Bounds().left, i * rowHeight, Bounds().right, (i + 1) * rowHeight - 1);
            const bool selected = i == fSelected;
            if (selected || i == fHovered) {
                SetHighColor(selected ? selectedBackground : Mix(background, selectedBackground, 0.35f));
                FillRect(frame);
            }
            const float iconLeft = frame.left + padding;
            const float iconTop = frame.top + std::floor((rowHeight - iconSize) / 2);
            BRect iconFrame(iconLeft, iconTop, iconLeft + iconSize - 1, iconTop + iconSize - 1);
            if (row.icon) {
                SetDrawingMode(B_OP_ALPHA);
                DrawBitmap(row.icon.get(), row.icon->Bounds(), iconFrame, B_FILTER_BITMAP_BILINEAR);
            } else {
                DrawGlyph(row.kind, iconFrame, selected ? selectedText : Mix(background, text, 0.55f));
            }
            const float baseline = frame.top + std::floor((rowHeight + height.ascent - height.descent) / 2);
            float x = iconFrame.right + padding;
            const float right = frame.right - padding;
            std::string title = row.title.empty() ? row.detail : row.title;
            BString shown(title.c_str());
            TruncateString(&shown, B_TRUNCATE_END, std::max(0.0f, (right - x) * (row.title.empty() ? 1 : 0.65f)));
            SetHighColor(selected ? selectedText : text);
            DrawString(shown.String(), BPoint(x, baseline));
            x += StringWidth(shown.String());
            if (!row.title.empty() && !row.detail.empty() && x < right) {
                BString detail = (std::string(" — ") + row.detail).c_str();
                TruncateString(&detail, B_TRUNCATE_END, right - x);
                SetHighColor(selected ? Mix(selectedText, selectedBackground, 0.25f)
                    : row.kind == SuggestionRow::Kind::Search ? Mix(background, text, 0.55f) : link);
                DrawString(detail.String(), BPoint(x, baseline));
            }
        }
        SetHighColor(Mix(background, text, 0.3f));
        StrokeRect(Bounds());
    }

    void MouseMoved(BPoint where, uint32 transit, const BMessage*) override
    {
        const int32 row = transit == B_EXITED_VIEW || transit == B_OUTSIDE_VIEW ? -1 : RowAt(where);
        if (row == fHovered) return;
        fHovered = row;
        Invalidate();
    }
    void MouseDown(BPoint where) override
    {
        const int32 row = RowAt(where);
        if (row < 0) return;
        BMessage chosen(kSuggestionChosen);
        chosen.AddInt32("index", row);
        chosen.AddString("url", fRows[row].url.c_str());
        fOwner.SendMessage(&chosen);
    }

private:
    int32 RowAt(BPoint where)
    {
        const int32 row = static_cast<int32>(where.y / RowHeight());
        return where.y >= 0 && row < static_cast<int32>(fRows.size()) ? row : -1;
    }
    // A clock for history, a star for bookmarks, a magnifier for a search.
    void DrawGlyph(SuggestionRow::Kind kind, BRect frame, rgb_color color)
    {
        SetHighColor(color);
        SetPenSize(std::max(1.0f, IconScale()));
        const BPoint center(frame.left + frame.Width() / 2, frame.top + frame.Height() / 2);
        const float radius = frame.Width() / 2 - 1;
        switch (kind) {
        case SuggestionRow::Kind::Search:
            StrokeEllipse(BPoint(center.x - radius * 0.2f, center.y - radius * 0.2f), radius * 0.6f, radius * 0.6f);
            StrokeLine(BPoint(center.x + radius * 0.25f, center.y + radius * 0.25f), BPoint(center.x + radius, center.y + radius));
            break;
        case SuggestionRow::Kind::Bookmark: {
            BPoint points[10];
            for (int i = 0; i < 10; ++i) {
                const float angle = -M_PI / 2 + i * M_PI / 5;
                const float r = i % 2 ? radius * 0.45f : radius;
                points[i] = BPoint(center.x + r * std::cos(angle), center.y + r * std::sin(angle));
            }
            StrokePolygon(points, 10);
            break;
        }
        case SuggestionRow::Kind::Visit:
        case SuggestionRow::Kind::History:
            StrokeEllipse(center, radius * 0.85f, radius * 0.85f);
            StrokeLine(center, BPoint(center.x, center.y - radius * 0.55f));
            StrokeLine(center, BPoint(center.x + radius * 0.4f, center.y));
            break;
        }
        SetPenSize(1);
    }

    BMessenger fOwner;
    std::vector<SuggestionRow> fRows;
    int32 fSelected = -1;
    int32 fHovered = -1;
};
}

class AddressSuggestions::ListWindow final : public BWindow {
public:
    explicit ListWindow(BWindow* owner)
        : BWindow(BRect(0, 0, 100, 100), "Suggestions", B_NO_BORDER_WINDOW_LOOK, B_FLOATING_SUBSET_WINDOW_FEEL,
              B_AVOID_FOCUS | B_NOT_MOVABLE | B_NOT_CLOSABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_RESIZABLE
                  | B_WILL_ACCEPT_FIRST_CLICK | B_ASYNCHRONOUS_CONTROLS)
    {
        fView = new ListView(BMessenger(owner));
        fView->SetResizingMode(B_FOLLOW_ALL);
        fView->ResizeTo(Bounds().Width(), Bounds().Height());
        AddChild(fView);
        AddToSubset(owner);
        // Run the window hidden, so later calls only need its lock.
        Hide();
        Show();
    }
    ListView* View() const { return fView; }

private:
    ListView* fView;
};

AddressSuggestions::AddressSuggestions(BWindow* owner)
    : fOwner(owner)
{
}

AddressSuggestions::~AddressSuggestions()
{
    if (fWindow && fWindow->Lock()) fWindow->Quit();
}

void AddressSuggestions::Show(BRect field, std::vector<SuggestionRow> rows)
{
    if (rows.empty()) {
        Hide();
        return;
    }
    if (static_cast<int32>(rows.size()) > kMaximumRows) rows.resize(kMaximumRows);
    fRows = rows;
    fSelected = -1;
    if (!fWindow) fWindow = new ListWindow(fOwner);
    if (!fWindow->Lock()) return;
    auto* view = fWindow->View();
    const float height = view->RowHeight() * rows.size();
    view->SetRows(std::move(rows), fSelected);
    // Under the field, as wide as it, kept on the field's screen.
    BRect screen = BScreen(fOwner).Frame();
    float width = std::max(field.Width(), 320.0f);
    float left = std::clamp(field.left, screen.left, std::max(screen.left, screen.right - width));
    fWindow->MoveTo(left, field.bottom + 2);
    fWindow->ResizeTo(width, height);
    if (fWindow->IsHidden()) fWindow->Show();
    fWindow->Unlock();
    fShowing = true;
}

void AddressSuggestions::Hide()
{
    fRows.clear();
    fSelected = -1;
    if (!fShowing || !fWindow) {
        fShowing = false;
        return;
    }
    fShowing = false;
    if (!fWindow->Lock()) return;
    if (!fWindow->IsHidden()) fWindow->Hide();
    fWindow->View()->SetRows({}, -1);
    fWindow->Unlock();
}

void AddressSuggestions::Select(int32 index)
{
    if (index < -1 || index >= static_cast<int32>(fRows.size())) index = -1;
    fSelected = index;
    if (fWindow && fWindow->Lock()) {
        fWindow->View()->SetSelected(index);
        fWindow->Unlock();
    }
}
}
