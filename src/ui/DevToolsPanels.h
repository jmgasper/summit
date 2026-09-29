#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "SourceView.h"
#include "core/DevTools.h"
#include <View.h>
#include <map>
#include <string>
#include <vector>

class BButton;
class BCardLayout;
class BCheckBox;
class BColumnListView;
class BMenuField;
class BRow;
class BStringView;
class BTabView;
class BTextControl;

// The two panels of the Developer Tools window (DevToolsWindow.h). They show
// one devtools::Session, which the window owns and feeds; all of it happens
// on the window's thread.
namespace summit {
// What a panel asks of the window after it has changed the session: send
// what the session wants sent, and show what changed in both panels.
class DevToolsHost {
public:
    virtual void Sync() = 0;
protected:
    ~DevToolsHost() = default;
};

// A request's or response's body: formatted when it is JSON, XML or HTML,
// coloured by its kind, with a button that copies what is shown.
class BodyView : public BView {
public:
    explicit BodyView(const char* name);
    void AttachedToWindow() override;
    void MessageReceived(BMessage* message) override;
    // The body's bytes as they were sent, and its Content-Type.
    void ShowBody(const std::string& bytes, const std::string& mimeType);
    // No body to show, and why.
    void ShowNote(const std::string& note);
    // What Copy copies: the text as it is shown.
    std::string ShownText();
    // As the check boxes and the button do.
    void SetFormatted(bool formatted);
    void SetWrapped(bool wrapped);
    void CopyShown();
    // {"info", "note", "text", "formatted", "shows"}, for tests.
    std::string StateJSON();

private:
    void Update();
    class ImageView;
    std::string fBytes, fMimeType;
    devtools::BodyKind fKind = devtools::BodyKind::Text;
    bool fHasBody = false;
    BCheckBox* fFormat;
    BCheckBox* fWrap;
    BButton* fCopy;
    BStringView* fInfo;
    SourceView* fSource;
    ImageView* fImage;
    BStringView* fNote;
    BCardLayout* fCards;
};

class NetworkPanel : public BView {
public:
    NetworkPanel(devtools::Session& session, DevToolsHost& host);
    void AttachedToWindow() override;
    void MessageReceived(BMessage* message) override;
    void Apply(const devtools::Changes& changes);
    // kDeveloperToolsCommand: false if the action is not the panel's; error
    // says what was wrong with one that is.
    bool Command(const std::string& action, const std::string& argument, std::string& error);
    std::string StateJSON();

private:
    class Row;
    void Select(Row* row);
    bool Shows(const devtools::Request& request) const;
    void Rebuild();
    void Fill(Row& row, const devtools::Request& request);
    void ShowSelection();
    void ShowBody(const devtools::Request& request);
    void ShowSummary();
    bool AtEnd() const;
    devtools::Session& fSession;
    DevToolsHost& fHost;
    std::map<uint64, Row*> fRows;
    uint64 fSelected = 0;
    std::string fFilterText;
    // Of the types menu; 0 is every type.
    int32 fCategory = 0;
    BButton* fClear;
    BCheckBox* fPreserve;
    BTextControl* fFilter;
    BMenuField* fTypes;
    BColumnListView* fList;
    BStringView* fSummary;
    BCardLayout* fDetailCards;
    BTabView* fDetails;
    SourceView* fHeaders;
    BodyView* fRequestBody;
    BodyView* fResponseBody;
};

class ConsolePanel : public BView {
public:
    ConsolePanel(devtools::Session& session, DevToolsHost& host);
    void AttachedToWindow() override;
    void MessageReceived(BMessage* message) override;
    void Apply(const devtools::Changes& changes);
    // A line of the input's history: older for -1, newer for 1.
    void StepHistory(int direction);
    bool Command(const std::string& action, const std::string& argument, std::string& error);
    std::string StateJSON();

private:
    class Log;
    bool Shows(const devtools::ConsoleEntry& entry) const;
    void Rebuild();
    void ShowCounts();
    devtools::Session& fSession;
    DevToolsHost& fHost;
    devtools::LevelFilter fLevels;
    std::string fFilterText;
    std::vector<std::string> fHistory;
    size_t fHistoryIndex = 0;
    // The serial of the last entry the log shows, to redraw it when it repeats.
    uint64 fLastShown = 0;
    BButton* fClear;
    BCheckBox* fPreserve;
    BCheckBox* fErrors;
    BCheckBox* fWarnings;
    BCheckBox* fInfo;
    BCheckBox* fDebug;
    BTextControl* fFilter;
    BStringView* fCounts;
    Log* fLog;
    BTextControl* fInput;
};
}
#endif
