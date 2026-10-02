#pragma once
#include <Messenger.h>
#include <Rect.h>
#include <memory>
#include <string>
#include <vector>

class BBitmap;
class BWindow;

namespace summit {
// One line of the list under the address field.
struct SuggestionRow {
    enum class Kind { Visit, Search, History, Bookmark };
    Kind kind = Kind::History;
    std::string title;
    // Dimmer text after the title: the short address, or "Search with …".
    std::string detail;
    // Where choosing the row goes.
    std::string url;
    // What the address field shows while the row is selected with the arrow keys.
    std::string fill;
    // The site's icon (copied), or null.
    std::shared_ptr<BBitmap> icon;
};

// The list of suggestions under the address field (Firefox's address bar
// list). It is a window of its own that never takes the focus: the address
// field keeps it, and its window moves the selection with the arrow keys. A
// click on a row sends kSuggestionChosen ("index") to the browser window.
class AddressSuggestions {
public:
    explicit AddressSuggestions(BWindow* owner);
    ~AddressSuggestions();
    AddressSuggestions(const AddressSuggestions&) = delete;
    AddressSuggestions& operator=(const AddressSuggestions&) = delete;

    // Shows rows under the address field (its frame in screen coordinates).
    // No rows hides the list.
    void Show(BRect fieldScreenFrame, std::vector<SuggestionRow> rows);
    void Hide();
    bool IsShowing() const { return fShowing; }
    // The selected row, -1 for none.
    int32 Selected() const { return fSelected; }
    void Select(int32 index);
    const std::vector<SuggestionRow>& Rows() const { return fRows; }

private:
    class ListWindow;
    BWindow* fOwner;
    ListWindow* fWindow = nullptr;
    std::vector<SuggestionRow> fRows;
    int32 fSelected = -1;
    bool fShowing = false;
};
}
