#pragma once
#include "core/ExtensionCatalog.h"
#include "core/ExtensionStore.h"
#include <atomic>

namespace summit {
// Downloads into a private temporary directory beside the profile, checks the
// store metadata, stages an immutable copy, and removes all temporary files.
// Runs on the installer's worker; cancellation also aborts network transfers.
std::unique_ptr<StagedExtensionPackage> DownloadStoreExtension(const ExtensionStoreItem&,
    const std::filesystem::path& temporaryRoot, const ExtensionCatalog&, const std::atomic<bool>& cancelled,
    std::string& expectedIdentity, std::string& error);
}
