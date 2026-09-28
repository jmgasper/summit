#pragma once
#include <SupportDefs.h>
namespace summit {
constexpr uint32 kNavigate = 'navi';
constexpr uint32 kNewTab = 'ntab';
constexpr uint32 kCreateTabOnApp = 'atap';
constexpr uint32 kCloseTab = 'ctab';
constexpr uint32 kRequestWindowClose = 'rwcl';
constexpr uint32 kWindowReadyToClose = 'wrcl';
constexpr uint32 kSelectTab = 'stab';
constexpr uint32 kBack = 'back';
constexpr uint32 kForward = 'frwd';
constexpr uint32 kReload = 'reld';
constexpr uint32 kHome = 'home';
constexpr uint32 kFocusAddress = 'fadr';
constexpr uint32 kBookmark = 'bkmk';
constexpr uint32 kShowBookmarks = 'sbkm';
constexpr uint32 kShowHistory = 'shis';
constexpr uint32 kOpenBookmark = 'obkm';
constexpr uint32 kAddToBookmarksBar = 'abkb';
constexpr uint32 kRemoveFromBookmarksBar = 'rbkb';
constexpr uint32 kRemoveBookmark = 'dbkm';
constexpr uint32 kBookmarkButton = 'bkbt';
constexpr uint32 kToggleBookmarksBar = 'tbkb';
constexpr uint32 kClearHistory = 'clhi';
constexpr uint32 kClearHistoryReply = 'clhr';
constexpr uint32 kShowPreferences = 'pref';
constexpr uint32 kPreferencesChanged = 'prch';
constexpr uint32 kPreferencesUseCurrentPage = 'prcp';
constexpr uint32 kPreferencesState = 'prst';
constexpr uint32 kPreferencesClosed = 'prcl';
// Preferences › History and Data, sent to the application; it answers the
// window with kDataCleared ("kind": the request, "error" on failure).
constexpr uint32 kClearHistoryRequest = 'pclh';
constexpr uint32 kClearCacheRequest = 'pclc';
constexpr uint32 kClearSiteDataRequest = 'pcld';
constexpr uint32 kDataCleared = 'pdcl';
constexpr uint32 kFind = 'find';
constexpr uint32 kFindNext = 'fnxt';
constexpr uint32 kFindPrevious = 'fprv';
constexpr uint32 kCloseFind = 'cfin';
constexpr uint32 kZoomIn = 'zmin';
constexpr uint32 kZoomOut = 'zmot';
constexpr uint32 kZoomReset = 'zres';
constexpr uint32 kSaveSession = 'sess';
constexpr uint32 kNextTab = 'next';
constexpr uint32 kPreviousTab = 'prev';
constexpr uint32 kReopenTab = 'rtab';
constexpr uint32 kOpenFile = 'file';
constexpr uint32 kShowDownloads = 'down';
constexpr uint32 kDownloadSave = 'dsav';
constexpr uint32 kDownloadQuitReply = 'dqre';
constexpr uint32 kShowExtensions = 'exts';
constexpr uint32 kInspectExtension = 'exin';
constexpr uint32 kExtensionSelected = 'exsl';
constexpr uint32 kExtensionsState = 'exst';
constexpr uint32 kExtensionApprove = 'exap';
constexpr uint32 kExtensionCancel = 'exca';
constexpr uint32 kExtensionEnable = 'exen';
constexpr uint32 kExtensionRemove = 'exrm';
constexpr uint32 kExtensionManagerClosed = 'exmc';
constexpr uint32 kCloseExtensionManager = 'excl';
constexpr uint32 kActivateExtensionAction = 'exac';
constexpr uint32 kShowExtensionActions = 'exam';
constexpr uint32 kBrowserState = 'stat';
// Windows. kNewWindow goes to the application ("url": repeated, optional;
// "new_page"/"new_page_url": a page opened by another page; "popup": bool;
// "frame": BRect). A window's close box asks the application with
// kCloseWindowRequest ("window"); the application answers with
// kRequestWindowClose ("quitting": bool) and the window reports
// kWindowReadyToClose or kWindowCloseCancelled.
constexpr uint32 kNewWindow = 'nwin';
// A private window (kNewWindow with "private" true). Its pages use a separate,
// in-memory website data store and leave no history.
constexpr uint32 kNewPrivateWindow = 'npwn';
constexpr uint32 kCloseWindow = 'cwin';
constexpr uint32 kCloseWindowRequest = 'cwrq';
constexpr uint32 kWindowCloseCancelled = 'wccn';
constexpr uint32 kWindowActivated = 'wact';
constexpr uint32 kActivateWindow = 'awin';
constexpr uint32 kMoveTabToNewWindow = 'mtnw';
constexpr uint32 kDuplicateTab = 'dtab';
constexpr uint32 kCloseOtherTabs = 'cotb';
constexpr uint32 kReloadTab = 'rtbx';
constexpr uint32 kProfileChanged = 'prfc';
// Page context menu commands ("url" names the link, image or media).
constexpr uint32 kOpenLink = 'olnk';
constexpr uint32 kOpenLinkInNewTab = 'olnt';
constexpr uint32 kOpenLinkInNewWindow = 'olnw';
constexpr uint32 kOpenLinkInNewPrivateWindow = 'olnp';
constexpr uint32 kCopyText = 'cptx';
constexpr uint32 kDownloadLink = 'dlnk';
constexpr uint32 kSaveLinkAs = 'slas';
constexpr uint32 kSaveLinkAsChosen = 'slac';
constexpr uint32 kSearchFor = 'srch';
constexpr uint32 kBookmarkLink = 'bklk';
constexpr uint32 kInterfaceStyle = 'istl';
// An extension's item in a page context menu ("token").
constexpr uint32 kExtensionMenuItem = 'exmi';
// Benchmark input synthesis, refused unless SUMMIT_ENABLE_INPUT_SYNTHESIS=1.
constexpr uint32 kSimulateScroll = 'sscr';
constexpr uint32 kScrollBurstCompleted = 'ssco';
constexpr uint32 kFrameStats = 'fsts';
}
