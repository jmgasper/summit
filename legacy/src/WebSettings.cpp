/*
 * Copyright 2026 Summit contributors. Distributed under the terms of the MIT License.
 *
 * BWebSettings on Summit's engine. The persistent storage path chooses where
 * the engine keeps the application's website data; site icons come from the
 * engine. Fonts, script and proxy settings are kept but not applied: the
 * engine has no per-application settings for them.
 */
#include "Private.h"

#include <Font.h>

namespace {
BWebSettings* sDefault = nullptr;
std::mutex sDefaultLock;

BString
FontName(const BFont& font)
{
	font_family family;
	font_style style;
	font.GetFamilyAndStyle(&family, &style);
	return BString(family);
}
}


BWebSettings::BWebSettings()
	:
	BHandler("BWebSettings"),
	fData(new BPrivate::WebSettingsPrivate())
{
}


BWebSettings::BWebSettings(WebCore::Settings*)
	:
	BHandler("BWebSettings"),
	fData(new BPrivate::WebSettingsPrivate())
{
}


BWebSettings::~BWebSettings()
{
	delete fData;
}


/*static*/ BWebSettings*
BWebSettings::Default()
{
	std::lock_guard lock(sDefaultLock);
	if (sDefault == nullptr)
		sDefault = new BWebSettings();
	return sDefault;
}


/*static*/ void
BWebSettings::SetPersistentStoragePath(const BString& path)
{
	SummitLegacy::SetStoragePath(path);
}


/*static*/ void
BWebSettings::SetIconDatabasePath(const BString&)
{
	// Icons are kept in memory, as the engine reports them.
}


/*static*/ void
BWebSettings::ClearIconDatabase()
{
	SummitLegacy::ClearIcons();
}


/*static*/ void
BWebSettings::SendIconForURL(const BString& url, const BMessage& reply, const BMessenger& target)
{
	BMessage message(reply);
	BMessage icon;
	if (SummitLegacy::ArchiveIcon(url, icon))
		message.AddMessage("icon", &icon);
	target.SendMessage(&message);
}


/*static*/ void BWebSettings::SetOfflineStoragePath(const BString&) { }
/*static*/ void BWebSettings::SetOfflineStorageDefaultQuota(int64) { }
/*static*/ void BWebSettings::SetOfflineWebApplicationCachePath(const BString&) { }
/*static*/ void BWebSettings::SetOfflineWebApplicationCacheQuota(int64) { }


void
BWebSettings::SetLocalStoragePath(const BString& path)
{
	std::lock_guard lock(fData->lock);
	fData->localStoragePath = path;
}


void
BWebSettings::SetSerifFont(const BFont& font)
{
	std::lock_guard lock(fData->lock);
	fData->serifFont = FontName(font);
}


void
BWebSettings::SetSansSerifFont(const BFont& font)
{
	std::lock_guard lock(fData->lock);
	fData->sansSerifFont = FontName(font);
}


void
BWebSettings::SetFixedFont(const BFont& font)
{
	std::lock_guard lock(fData->lock);
	fData->fixedFont = FontName(font);
}


void
BWebSettings::SetStandardFont(const BFont& font)
{
	std::lock_guard lock(fData->lock);
	fData->standardFont = FontName(font);
}


void
BWebSettings::SetDefaultStandardFontSize(float size)
{
	std::lock_guard lock(fData->lock);
	fData->standardFontSize = size;
}


void
BWebSettings::SetDefaultFixedFontSize(float size)
{
	std::lock_guard lock(fData->lock);
	fData->fixedFontSize = size;
}


void
BWebSettings::SetJavascriptEnabled(bool enable)
{
	std::lock_guard lock(fData->lock);
	fData->javascriptEnabled = enable;
}


/*static*/ void
BWebSettings::SetProxyInfo(const BString&, uint32, BProxyType, const BString&, const BString&)
{
	// The engine's network process uses the system's network settings.
}


void
BWebSettings::Apply()
{
}


void
BWebSettings::MessageReceived(BMessage* message)
{
	BHandler::MessageReceived(message);
}
