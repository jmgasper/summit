#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace summit {
// An engine-verified CRX signer identity takes precedence for CRX packages.
// The optional value must come from package verification, not manifest data.
// Otherwise a declared Gecko identity takes precedence for cross-browser manifests.
// Otherwise a Chrome manifest key determines the ID; unkeyed local packages
// retain the caller's installation identity. Malformed declarations throw.
std::string ExtensionIdentity(const nlohmann::json& manifest, std::string_view localIdentity, std::string_view verifiedCRXIdentity = { });
}
