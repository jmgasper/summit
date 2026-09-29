#if SUMMIT_MODERN_WEBKIT
#include "DevToolsPanels.h"
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <MessageFilter.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace summit {
using namespace devtools;
namespace {
constexpr uint32 kClearConsole = 'dccl', kPreserveConsole = 'dcpr', kLevelsChanged = 'dclv', kFilterConsole = 'dcfl',
    kEvaluate = 'dcev';

// Styles of the log. The lexers' are below 40 and the StyledText runs' from 64.
enum {
    kLogStyle = 90, kInfoStyle, kDebugStyle, kWarningStyle, kErrorStyle, kStackStyle, kWarningStackStyle,
    kErrorStackStyle, kInputStyle, kResultStyle, kFailedResultStyle, kNavigationStyle, kTimeStyle, kPlaceStyle,
    kWarningPlaceStyle, kErrorPlaceStyle
};
// Marks in the margin, one per severity: shape and colour both tell them apart.
enum { kErrorMark = 1, kWarningMark, kInfoMark, kDebugMark, kInputMark, kResultMark, kNavigationMark };

std::string LowerCase(std::string text)
{
    for (auto& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return text;
}

std::string ClockTime(double secondsSinceEpoch)
{
    if (secondsSinceEpoch <= 0) return { };
    const auto whole = static_cast<std::time_t>(secondsSinceEpoch);
    std::tm local { };
    localtime_r(&whole, &local);
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
        static_cast<int>((secondsSinceEpoch - static_cast<double>(whole)) * 1000) % 1000);
    return buffer;
}

sptr_t Pointer(const char* text) { return reinterpret_cast<sptr_t>(text); }

// Up and Down in the input bring back what was typed before.
class HistoryKeys : public BMessageFilter {
public:
    explicit HistoryKeys(ConsolePanel* panel)
        : BMessageFilter(B_ANY_DELIVERY, B_ANY_SOURCE, B_KEY_DOWN), fPanel(panel)
    {
    }
    filter_result Filter(BMessage* message, BHandler**) override
    {
        const char* bytes = nullptr;
        if (message->FindString("bytes", &bytes) != B_OK || !bytes) return B_DISPATCH_MESSAGE;
        if (bytes[0] != B_UP_ARROW && bytes[0] != B_DOWN_ARROW) return B_DISPATCH_MESSAGE;
        fPanel->StepHistory(bytes[0] == B_UP_ARROW ? -1 : 1);
        return B_SKIP_MESSAGE;
    }
private:
    ConsolePanel* fPanel;
};
}

// The console's text: one entry after another, each in the colours of its
// severity, with its time and a mark in the margin and its place in the
// page's sources after its first line.
class ConsolePanel::Log : public SourceView {
public:
    Log()
        : SourceView("log", false, false)
    {
        SendMessage(SCI_SETILEXER, 0, 0);
        SendMessage(SCI_SETMARGINTYPEN, 0, SC_MARGIN_SYMBOL);
        SendMessage(SCI_SETMARGINMASKN, 0, 0xFE);
        SendMessage(SCI_SETMARGINWIDTHN, 0, 20);
        SendMessage(SCI_SETMARGINTYPEN, 1, SC_MARGIN_RTEXT);
        SendMessage(SCI_SETMARGINMASKN, 1, 0);
        SendMessage(SCI_SETMARGINSENSITIVEN, 1, 0);
        SendMessage(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_STANDARD);
        SendMessage(SCI_SETENDATLASTLINE, 1);
        // A severity's background reaches the edge of the view.
        SendMessage(SCI_SETMARGINRIGHT, 0, 0);
        ApplyPalette();
    }
    void AllAttached() override
    {
        SourceView::AllAttached();
        ApplyPalette();
    }
    size_t Count() const { return fStarts.size(); }
    bool AtEnd()
    {
        const auto last = SendMessage(SCI_GETLINECOUNT) - 1;
        const auto end = SendMessage(SCI_VISIBLEFROMDOCLINE, last) + SendMessage(SCI_WRAPCOUNT, last);
        return SendMessage(SCI_GETFIRSTVISIBLELINE) + SendMessage(SCI_LINESONSCREEN) >= end;
    }
    void ScrollToEnd() { SendMessage(SCI_SCROLLTOEND); }
    void Clear()
    {
        SendMessage(SCI_SETREADONLY, 0);
        SendMessage(SCI_CLEARALL);
        SendMessage(SCI_SETREADONLY, 1);
        SendMessage(SCI_MARKERDELETEALL, static_cast<uptr_t>(-1));
        SendMessage(SCI_MARGINTEXTCLEARALL);
        SendMessage(SCI_EOLANNOTATIONCLEARALL);
        SendMessage(SCI_SETSCROLLWIDTH, 1);
        fStarts.clear();
        fLastStyle = kLogStyle;
    }
    void Append(const ConsoleEntry& entry)
    {
        int style = kLogStyle, stackStyle = kStackStyle, placeStyle = kPlaceStyle, mark = 0;
        std::string text = entry.text;
        switch (entry.kind) {
            case EntryKind::Input: style = kInputStyle; mark = kInputMark; break;
            case EntryKind::Result:
                style = entry.level == Level::Error ? kFailedResultStyle : kResultStyle;
                mark = entry.level == Level::Error ? kErrorMark : kResultMark;
                break;
            case EntryKind::Navigation: style = kNavigationStyle; mark = kNavigationMark; break;
            case EntryKind::Message:
                switch (entry.level) {
                    case Level::Error: style = kErrorStyle; stackStyle = kErrorStackStyle; placeStyle = kErrorPlaceStyle; mark = kErrorMark; break;
                    case Level::Warning: style = kWarningStyle; stackStyle = kWarningStackStyle; placeStyle = kWarningPlaceStyle; mark = kWarningMark; break;
                    case Level::Info: style = kInfoStyle; mark = kInfoMark; break;
                    case Level::Debug: style = kDebugStyle; mark = kDebugMark; break;
                    default: break;
                }
                break;
        }
        if (text.empty()) text = " ";
        if (entry.repeat > 1) text += "  (×" + std::to_string(entry.repeat) + ")";
        SendMessage(SCI_SETREADONLY, 0);
        // The line end belongs to the entry before: it carries that entry's
        // background to the edge of the view.
        if (SendMessage(SCI_GETLENGTH) > 0) Add("\n", fLastStyle);
        const auto start = SendMessage(SCI_GETLENGTH);
        fStarts.push_back(start);
        Add(text, style);
        if (!entry.stack.empty()) {
            std::string stack;
            for (size_t from = 0; from <= entry.stack.size();) {
                const auto end = std::min(entry.stack.find('\n', from), entry.stack.size());
                stack += "\n    " + entry.stack.substr(from, end - from);
                from = end + 1;
            }
            Add(stack, stackStyle);
            fLastStyle = stackStyle;
        } else fLastStyle = style;
        SendMessage(SCI_SETREADONLY, 1);
        const auto line = SendMessage(SCI_LINEFROMPOSITION, start);
        if (mark) SendMessage(SCI_MARKERADD, line, mark);
        if (const auto time = ClockTime(entry.time); !time.empty()) {
            SendMessage(SCI_MARGINSETTEXT, line, Pointer(time.c_str()));
            SendMessage(SCI_MARGINSETSTYLE, line, kTimeStyle);
        }
        if (const auto place = entry.Location(); !place.empty()) {
            SendMessage(SCI_EOLANNOTATIONSETTEXT, line, Pointer(place.c_str()));
            SendMessage(SCI_EOLANNOTATIONSETSTYLE, line, placeStyle);
        }
    }
    void RemoveLast()
    {
        if (fStarts.empty()) return;
        const auto start = fStarts.back();
        fStarts.pop_back();
        const auto first = SendMessage(SCI_LINEFROMPOSITION, start), last = SendMessage(SCI_GETLINECOUNT) - 1;
        // What is attached to a line would move to the line before it.
        for (auto line = first; line <= last; ++line) {
            SendMessage(SCI_MARKERDELETE, line, static_cast<sptr_t>(-1));
            SendMessage(SCI_MARGINSETTEXT, line, 0);
            SendMessage(SCI_EOLANNOTATIONSETTEXT, line, 0);
        }
        const auto from = std::max<sptr_t>(0, start - 1);
        SendMessage(SCI_SETREADONLY, 0);
        SendMessage(SCI_DELETERANGE, from, SendMessage(SCI_GETLENGTH) - from);
        SendMessage(SCI_SETREADONLY, 1);
        const auto length = SendMessage(SCI_GETLENGTH);
        fLastStyle = length > 0 ? static_cast<int>(SendMessage(SCI_GETSTYLEAT, length - 1)) : kLogStyle;
    }

protected:
    void ApplyPalette() override
    {
        SourceView::ApplyPalette();
        const auto& palette = CurrentSourcePalette();
        const auto style = [&](int number, rgb_color text, rgb_color back, bool bold = false, bool italic = false) {
            SendMessage(SCI_STYLESETFORE, number, SciColor(text));
            SendMessage(SCI_STYLESETBACK, number, SciColor(back));
            SendMessage(SCI_STYLESETBOLD, number, bold);
            SendMessage(SCI_STYLESETITALIC, number, italic);
            SendMessage(SCI_STYLESETEOLFILLED, number, 1);
        };
        const rgb_color amber = palette.dark ? rgb_color {245, 200, 110, 255} : rgb_color {138, 92, 0, 255};
        const auto errorBack = palette.Tint(palette.removed, palette.dark ? .20f : .11f);
        const auto warningBack = palette.Tint(rgb_color {240, 180, 40, 255}, palette.dark ? .18f : .16f);
        style(kLogStyle, palette.text, palette.background);
        style(kInfoStyle, palette.accent, palette.background);
        style(kDebugStyle, palette.muted, palette.background);
        style(kWarningStyle, amber, warningBack);
        style(kErrorStyle, palette.removed, errorBack, true);
        style(kStackStyle, palette.muted, palette.background);
        style(kWarningStackStyle, palette.Tint(amber, .75f), warningBack);
        style(kErrorStackStyle, palette.Tint(palette.removed, .8f), errorBack);
        style(kInputStyle, palette.keyword, palette.background, true);
        style(kResultStyle, palette.string, palette.background);
        style(kFailedResultStyle, palette.removed, errorBack);
        style(kNavigationStyle, palette.accent, palette.Tint(palette.accent, .10f), false, true);
        style(kTimeStyle, palette.muted, palette.background);
        style(kPlaceStyle, palette.muted, palette.background, false, true);
        style(kWarningPlaceStyle, palette.Tint(amber, .7f), warningBack, false, true);
        style(kErrorPlaceStyle, palette.Tint(palette.removed, .7f), errorBack, false, true);
        const auto mark = [&](int number, int symbol, rgb_color color) {
            SendMessage(SCI_MARKERDEFINE, number, symbol);
            SendMessage(SCI_MARKERSETFORE, number, SciColor(color));
            SendMessage(SCI_MARKERSETBACK, number, SciColor(color));
        };
        mark(kErrorMark, SC_MARK_CIRCLE, palette.removed);
        mark(kWarningMark, SC_MARK_ARROWDOWN, rgb_color {224, 160, 20, 255});
        mark(kInfoMark, SC_MARK_ROUNDRECT, palette.accent);
        mark(kDebugMark, SC_MARK_SMALLRECT, palette.muted);
        mark(kInputMark, SC_MARK_SHORTARROW, palette.keyword);
        mark(kResultMark, SC_MARK_MINUS, palette.string);
        mark(kNavigationMark, SC_MARK_ARROWS, palette.accent);
        SendMessage(SCI_SETMARGINBACKN, 0, SciColor(palette.background));
        SendMessage(SCI_SETMARGINWIDTHN, 1, SendMessage(SCI_TEXTWIDTH, kTimeStyle, Pointer("00:00:00.000")) + 14);
    }

private:
    void Add(std::string_view text, int style)
    {
        const auto start = SendMessage(SCI_GETLENGTH);
        SendMessage(SCI_APPENDTEXT, text.size(), Pointer(text.data()));
        SendMessage(SCI_STARTSTYLING, start);
        SendMessage(SCI_SETSTYLING, text.size(), style);
    }
    std::vector<sptr_t> fStarts;
    int fLastStyle = kLogStyle;
};

ConsolePanel::ConsolePanel(Session& session, DevToolsHost& host)
    : BView("Console", 0), fSession(session), fHost(host)
{
    fClear = new BButton("clear", "Clear", new BMessage(kClearConsole));
    fClear->SetToolTip("Remove the messages shown");
    fPreserve = new BCheckBox("preserve", "Preserve log", new BMessage(kPreserveConsole));
    fPreserve->SetToolTip("Keep the messages of earlier pages when the page changes");
    const auto level = [](const char* name, const char* label) {
        auto* box = new BCheckBox(name, label, new BMessage(kLevelsChanged));
        box->SetValue(B_CONTROL_ON);
        return box;
    };
    fErrors = level("errors", "Errors");
    fWarnings = level("warnings", "Warnings");
    fInfo = level("info", "Info");
    fInfo->SetToolTip("console.info() and console.log()");
    fDebug = level("debug", "Debug");
    fFilter = new BTextControl("filter", "Filter:", "", nullptr);
    fFilter->SetModificationMessage(new BMessage(kFilterConsole));
    fFilter->TextView()->SetExplicitMinSize(BSize(120, B_SIZE_UNSET));
    fFilter->SetToolTip("Show messages that contain this");
    fCounts = new BStringView("counts", "");
    fCounts->SetTruncation(B_TRUNCATE_END);
    fCounts->SetExplicitMinSize(BSize(40, B_SIZE_UNSET));
    fCounts->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fCounts->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    fLog = new Log();
    fInput = new BTextControl("input", "›", "", new BMessage(kEvaluate));
    fInput->SetToolTip("Run JavaScript in the page");
    fInput->TextView()->SetFontAndColor(be_fixed_font);
    fInput->TextView()->AddFilter(new HistoryKeys(this));
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL, 8).SetInsets(8, 6, 8, 6)
            .Add(fClear)
            .Add(fPreserve)
            .AddStrut(8)
            .Add(fErrors)
            .Add(fWarnings)
            .Add(fInfo)
            .Add(fDebug)
            .Add(fFilter, 1)
        .End()
        .Add(fLog, 1)
        .AddGroup(B_HORIZONTAL, 0).SetInsets(8, 3, 8, 3)
            .Add(fCounts)
        .End()
        .AddGroup(B_HORIZONTAL, 0).SetInsets(8, 0, 8, 6)
            .Add(fInput)
        .End();
    ShowCounts();
}

void ConsolePanel::AttachedToWindow()
{
    BView::AttachedToWindow();
    AdoptParentColors();
    for (BControl* control : std::initializer_list<BControl*> {fClear, fPreserve, fErrors, fWarnings, fInfo, fDebug, fFilter, fInput})
        control->SetTarget(this);
}

void ConsolePanel::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kClearConsole:
            fSession.ClearConsole();
            fHost.Sync();
            break;
        case kPreserveConsole: fSession.SetPreserveConsole(fPreserve->Value() == B_CONTROL_ON); break;
        case kLevelsChanged:
            fLevels.errors = fErrors->Value() == B_CONTROL_ON;
            fLevels.warnings = fWarnings->Value() == B_CONTROL_ON;
            fLevels.info = fInfo->Value() == B_CONTROL_ON;
            fLevels.debug = fDebug->Value() == B_CONTROL_ON;
            Rebuild();
            break;
        case kFilterConsole: {
            const auto text = LowerCase(fFilter->Text());
            if (text == fFilterText) break;
            fFilterText = text;
            Rebuild();
            break;
        }
        case kEvaluate: {
            const std::string expression = fInput->Text();
            if (expression.find_first_not_of(" \t\r\n") == std::string::npos) break;
            if (fHistory.empty() || fHistory.back() != expression) fHistory.push_back(expression);
            if (fHistory.size() > 200) fHistory.erase(fHistory.begin());
            fHistoryIndex = fHistory.size();
            fInput->SetText("");
            fSession.Evaluate(expression);
            fHost.Sync();
            fLog->ScrollToEnd();
            break;
        }
        default: BView::MessageReceived(message);
    }
}

bool ConsolePanel::Command(const std::string& action, const std::string& argument, std::string&)
{
    const bool on = argument == "on";
    if (action == "clear-console") {
        fSession.ClearConsole();
        fHost.Sync();
    } else if (action == "preserve-console") {
        fPreserve->SetValue(on ? B_CONTROL_ON : B_CONTROL_OFF);
        fSession.SetPreserveConsole(on);
    } else if (action == "levels") {
        // The levels to show, by name: "errors,warnings".
        const auto has = [&](const char* name) { return argument.find(name) != std::string::npos; };
        fLevels = {has("errors"), has("warnings"), has("info"), has("debug")};
        fErrors->SetValue(fLevels.errors);
        fWarnings->SetValue(fLevels.warnings);
        fInfo->SetValue(fLevels.info);
        fDebug->SetValue(fLevels.debug);
        Rebuild();
    } else if (action == "filter-console") {
        fFilter->SetText(argument.c_str());
        fFilterText = LowerCase(argument);
        Rebuild();
    } else if (action == "evaluate") {
        fInput->SetText(argument.c_str());
        BMessage evaluate(kEvaluate);
        MessageReceived(&evaluate);
    } else return false;
    return true;
}

std::string ConsolePanel::StateJSON()
{
    using nlohmann::json;
    auto text = fLog->Text();
    if (text.size() > 256 * 1024) text.erase(0, text.size() - 256 * 1024);
    json state = {{"preserve", fPreserve->Value() == B_CONTROL_ON}, {"filter", fFilter->Text()},
        {"levels", {{"errors", fLevels.errors}, {"warnings", fLevels.warnings}, {"info", fLevels.info}, {"debug", fLevels.debug}}},
        {"shown", fLog->Count()}, {"counts", fCounts->Text()}, {"text", text}};
    return state.dump(-1, ' ', false, json::error_handler_t::replace);
}

void ConsolePanel::StepHistory(int direction)
{
    if (fHistory.empty()) return;
    if (direction < 0 && fHistoryIndex > 0) --fHistoryIndex;
    else if (direction > 0 && fHistoryIndex < fHistory.size()) ++fHistoryIndex;
    else return;
    const auto text = fHistoryIndex < fHistory.size() ? fHistory[fHistoryIndex] : std::string();
    fInput->SetText(text.c_str());
    fInput->TextView()->Select(static_cast<int32>(text.size()), static_cast<int32>(text.size()));
}

bool ConsolePanel::Shows(const ConsoleEntry& entry) const
{
    if (!fLevels.Shows(entry)) return false;
    return fFilterText.empty() || LowerCase(entry.text).find(fFilterText) != std::string::npos;
}

void ConsolePanel::Rebuild()
{
    fLog->Clear();
    fLastShown = 0;
    for (const auto& entry : fSession.Entries()) {
        if (!Shows(entry)) continue;
        fLog->Append(entry);
        fLastShown = entry.serial;
    }
    fLog->ScrollToEnd();
    ShowCounts();
}

void ConsolePanel::Apply(const Changes& changes)
{
    if (changes.consoleReset) {
        Rebuild();
        return;
    }
    if (!changes.consoleAdded && !changes.consoleLastChanged) return;
    const auto& entries = fSession.Entries();
    const bool follow = fLog->AtEnd();
    if (changes.consoleLastChanged && !entries.empty()) {
        const auto& last = entries[entries.size() - 1 - std::min(changes.consoleAdded, entries.size() - 1)];
        if (last.serial == fLastShown) {
            fLog->RemoveLast();
            fLog->Append(last);
        }
    }
    for (size_t i = entries.size() - std::min(changes.consoleAdded, entries.size()); i < entries.size(); ++i) {
        if (!Shows(entries[i])) continue;
        fLog->Append(entries[i]);
        fLastShown = entries[i].serial;
    }
    if (follow) fLog->ScrollToEnd();
    ShowCounts();
}

void ConsolePanel::ShowCounts()
{
    size_t errors = 0, warnings = 0, messages = 0;
    for (const auto& entry : fSession.Entries()) {
        if (entry.kind != EntryKind::Message) continue;
        ++messages;
        if (entry.level == Level::Error) ++errors;
        else if (entry.level == Level::Warning) ++warnings;
    }
    if (fSession.Entries().empty()) {
        fCounts->SetText("No messages.");
        return;
    }
    const auto count = [](size_t number, const char* one, const char* many) {
        return std::to_string(number) + " " + (number == 1 ? one : many);
    };
    auto text = count(messages, "message", "messages");
    if (errors) text += " · " + count(errors, "error", "errors");
    if (warnings) text += " · " + count(warnings, "warning", "warnings");
    if (const auto hidden = fSession.Entries().size() - fLog->Count()) text += " · " + std::to_string(hidden) + " hidden";
    fCounts->SetText(text.c_str());
}
}
#endif
