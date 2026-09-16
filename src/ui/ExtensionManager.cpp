#include "ExtensionManager.h"
#if SUMMIT_MODERN_WEBKIT
#include "ExtensionInstaller.h"
#include "Messages.h"
#include <Button.h>
#include <CheckBox.h>
#include <FilePanel.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListItem.h>
#include <ListView.h>
#include <Message.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextView.h>

namespace summit {
namespace {
constexpr uint32 choosePackage = 'exfp', selectEntry = 'exse';
std::string field(const BMessage& message, const char* name)
{
    const char* value = nullptr;
    return message.FindString(name, &value) == B_OK && value ? value : "";
}
class Item final : public BStringItem {
public:
    explicit Item(const BMessage& entry)
        : BStringItem(ExtensionDisplayText(field(entry, "name")).c_str()), identifier(field(entry, "identifier"))
        , name(field(entry, "name")), version(field(entry, "version")), error(field(entry, "error"))
    {
        entry.FindBool("enabled", &enabled);
        entry.FindBool("loaded", &loaded);
        entry.FindBool("installed", &installed);
    }
    std::string identifier, name, version, error;
    bool enabled = false, loaded = false, installed = true;
};
}
ExtensionManager::ExtensionManager(BMessenger owner, std::shared_ptr<std::atomic<unsigned>> windows)
    : BWindow(BRect(130, 110, 1040, 680), "Extensions — Summit", B_TITLED_WINDOW,
        B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , fOwner(owner), fWindows(std::move(windows))
{
    ++*fWindows;
    auto* title = new BStringView("extensions-title", "Extensions");
    title->SetFont(be_bold_font);
    fAdd = new BButton("extension-add", "Add extension…", new BMessage(choosePackage));
    fList = new BListView("extension-list");
    fList->SetSelectionMessage(new BMessage(selectEntry));
    auto* list = new BScrollView("extension-list-scroll", fList, 0, false, true);
    list->SetExplicitMinSize(BSize(220, 250));
    list->SetExplicitMaxSize(BSize(280, B_SIZE_UNLIMITED));
    fHeading = new BStringView("extension-heading", "No extension selected");
    fHeading->SetFont(be_bold_font);
    fDetails = new BTextView("extension-details");
    fDetails->MakeEditable(false);
    fDetails->MakeSelectable(true);
    fDetails->SetWordWrap(true);
    fDetails->SetInsets(12, 12, 12, 12);
    fDetails->SetViewColor(ui_color(B_DOCUMENT_BACKGROUND_COLOR));
    fDetails->SetLowColor(fDetails->ViewColor());
    auto color = ui_color(B_DOCUMENT_TEXT_COLOR);
    fDetails->SetFontAndColor(be_plain_font, B_FONT_ALL, &color);
    auto* details = new BScrollView("extension-details-scroll", fDetails, 0, false, true);
    details->SetExplicitMinSize(BSize(420, 260));
    fToggle = new BButton("extension-toggle", "Enable", new BMessage(kExtensionEnable));
    fRemove = new BButton("extension-remove", "Remove", new BMessage(kExtensionRemove));
    fStatus = new BStringView("extension-status", "");
    fStatus->SetTruncation(B_TRUNCATE_END);
    fFiles = new BCheckBox("extension-files", "Allow access to local files", new BMessage());
    fInstall = new BButton("extension-install", "Install", new BMessage(kExtensionApprove));
    fCancel = new BButton("extension-cancel", "Cancel installation", new BMessage(kExtensionCancel));
    fApproval = new BGroupView(B_VERTICAL, 8);
    BLayoutBuilder::Group<>(fApproval)
        .Add(fFiles)
        .AddGroup(B_HORIZONTAL, 8).AddGlue().Add(fCancel).Add(fInstall).End();
    BLayoutBuilder::Group<>(this, B_VERTICAL, 12)
        .SetInsets(16)
        .AddGroup(B_HORIZONTAL).Add(title).AddGlue().Add(fAdd).End()
        .AddGroup(B_HORIZONTAL, 16)
            .Add(list)
            .AddGroup(B_VERTICAL, 8)
                .Add(fHeading).Add(details)
                .AddGroup(B_HORIZONTAL, 8).Add(fToggle).Add(fRemove).AddGlue().End()
                .Add(fApproval)
            .End()
        .End()
        .Add(fStatus);
    fApproval->Hide();
    Render();
}
ExtensionManager::~ExtensionManager()
{
    fPanel.reset();
    while (auto* item = fList->RemoveItem(int32(0))) delete item;
    --*fWindows;
}
void ExtensionManager::Send(BMessage& message)
{
    message.AddMessenger("window", BMessenger(this));
    if (fOwner.SendMessage(&message, static_cast<BHandler*>(nullptr), 0) != B_OK)
        fStatus->SetText("Could not send the extension action. Please try again.");
}
bool ExtensionManager::QuitRequested()
{
    BMessage closed(kExtensionManagerClosed);
    closed.AddUInt64("generation", fGeneration);
    Send(closed);
    return true;
}
void ExtensionManager::Apply(const BMessage& state)
{
    auto* selected = dynamic_cast<Item*>(fList->ItemAt(fList->CurrentSelection()));
    const auto identifier = selected ? selected->identifier : std::string();
    while (auto* item = fList->RemoveItem(int32(0))) delete item;
    BMessage entry;
    int32 selection = -1;
    for (int32 i = 0; state.FindMessage("entry", i, &entry) == B_OK; ++i) {
        auto* item = new Item(entry);
        fList->AddItem(item);
        if (item->identifier == identifier) selection = i;
    }
    if (selection < 0 && fList->CountItems()) selection = 0;
    if (selection >= 0) fList->Select(selection);
    state.FindBool("ready", &fReady);
    state.FindBool("import_busy", &fBusy);
    state.FindBool("approval_ready", &fApprovalReady);
    uint64 generation = 0;
    state.FindUInt64("generation", &generation);
    if (generation != fGeneration) fFiles->SetValue(B_CONTROL_OFF);
    fGeneration = generation;
    fDraftName = field(state, "draft_name");
    fDraftBody = field(state, "draft_body");
    fStatusText = field(state, "import_status");
    if (!field(state, "catalog_error").empty()) fStatusText = field(state, "catalog_error");
    Render();
}
void ExtensionManager::Render()
{
    auto* item = dynamic_cast<Item*>(fList->ItemAt(fList->CurrentSelection()));
    fAdd->SetEnabled(fReady && !fBusy);
    fToggle->SetEnabled(fReady && !fBusy && item && item->installed);
    fRemove->SetEnabled(fReady && !fBusy && item);
    fToggle->SetLabel(item && (item->loaded || item->enabled) ? "Disable" : "Enable");
    fInstall->SetEnabled(fApprovalReady);
    fFiles->SetEnabled(fApprovalReady);
    fCancel->SetEnabled(fBusy);
    if (fBusy && fApproval->IsHidden()) fApproval->Show();
    else if (!fBusy && !fApproval->IsHidden()) fApproval->Hide();
    if (fBusy) {
        fHeading->SetText(fDraftName.empty() ? "Add extension" : ("Install " + fDraftName + "?").c_str());
        fDetails->SetText(fDraftBody.empty() ? fStatusText.c_str() : fDraftBody.c_str());
    } else if (item) {
        fHeading->SetText(ExtensionDisplayText(item->name).c_str());
        std::string body = "Version " + ExtensionDisplayText(item->version) + "\n"
            + (item->loaded ? "Running" : item->enabled ? "Enabled, but not running" : "Disabled")
            + "\nIdentifier: " + ExtensionDisplayText(item->identifier);
        if (!item->installed) body += "\nThis temporary runtime could not be saved as an installation.";
        if (!item->error.empty()) body += "\n\n" + ExtensionDisplayText(item->error);
        body += "\n\nRemoving stops this extension and removes it from startup. Its package and saved data stay in this profile.";
        fDetails->SetText(body.c_str());
    } else {
        fHeading->SetText("No extensions installed");
        fDetails->SetText("Add a CRX3, ZIP or XPI package, or an unpacked extension folder. You can review its requested access before installing it.");
    }
    fStatus->SetText(ExtensionDisplayText(fStatusText).c_str());
    fStatus->SetToolTip(fStatusText.c_str());
}
void ExtensionManager::MessageReceived(BMessage* message)
{
    if (message->what == kExtensionsState) { Apply(*message); return; }
    if (message->what == kShowExtensions) { Activate(); return; }
    if (message->what == kCloseExtensionManager) { Quit(); return; }
    if (message->what == selectEntry) { Render(); return; }
    if (message->what == choosePackage) {
        if (!fReady || fBusy) return;
        if (!fPanel) {
            BMessenger target(this);
            BMessage selected(kExtensionSelected);
            fPanel = std::make_unique<BFilePanel>(B_OPEN_PANEL, &target, nullptr,
                B_FILE_NODE | B_DIRECTORY_NODE, false, &selected);
            fPanel->SetButtonLabel(B_DEFAULT_BUTTON, "Review extension");
            fPanel->Window()->SetTitle("Choose extension package — Summit");
        }
        fPanel->Show();
        return;
    }
    if (message->what == kExtensionSelected) {
        if (fReady && !fBusy) {
            BMessage request(*message);
            Send(request);
        }
        return;
    }
    if (message->what == kExtensionApprove || message->what == kExtensionCancel) {
        BMessage request(message->what);
        request.AddUInt64("generation", fGeneration);
        request.AddBool("allow_files", fFiles->Value() == B_CONTROL_ON);
        Send(request);
        return;
    }
    if (message->what == kExtensionEnable || message->what == kExtensionRemove) {
        auto* item = dynamic_cast<Item*>(fList->ItemAt(fList->CurrentSelection()));
        if (!item || !fReady || fBusy) return;
        BMessage request(message->what);
        request.AddString("extension_identifier", item->identifier.c_str());
        request.AddBool("enabled", !(item->loaded || item->enabled));
        Send(request);
        return;
    }
    BWindow::MessageReceived(message);
}
}
#endif
