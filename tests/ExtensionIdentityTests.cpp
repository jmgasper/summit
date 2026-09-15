#include "core/ExtensionIdentity.h"
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>

using nlohmann::json;
static int checks, failures;
static void check(bool okay, const char* label)
{
    ++checks;
    if (!okay) ++failures;
    std::printf("%s %s\n", okay ? "PASS" : "FAIL", label);
}
static void rejects(const json& manifest, const char* label)
{
    try { summit::ExtensionIdentity(manifest, "summit-pkg-abc123"); check(false, label); }
    catch (const std::exception&) { check(true, label); }
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    std::ifstream input(argv[1]);
    std::string publicKey;
    std::getline(input, publicKey);
    if (!input || publicKey.empty()) return 2;
    const auto id = [](const json& manifest) { return summit::ExtensionIdentity(manifest, "summit-pkg-abc123"); };
    // Expected IDs and public key are Chromium's independent IDUtilTest vectors;
    // see fixtures/extensions/chrome-key/README.md for their source.
    check(id({{"key", publicKey}}) == "melddjfinppjdikinhbgehiennejpfhp", "Chromium public-key vector has the same extension ID");
    check(id({{"key", "dGVzdA=="}}) == "jpignaibiiemhngfjkcpokkamffknabf", "Chromium test-bytes vector matches");
    check(id({{"key", "Xw=="}}) == "ncocknphbhhlhkikpnnlmbcnbgdempcd", "Chromium underscore vector matches");
    check(id({{"key", "-----BEGIN PUBLIC KEY-----\n" + publicKey.substr(0, 64) + "\r\n" + publicKey.substr(64) + "\n-----END PUBLIC KEY-----\n"}})
        == "melddjfinppjdikinhbgehiennejpfhp", "PEM wrapping preserves the public-key identity");
    check(id(json::object()) == "summit-pkg-abc123", "unkeyed package retains its installation identity");
    check(id({{"applications", {{"gecko", {{"id", "legacy@example.test"}}}}}}) == "legacy@example.test", "legacy Gecko identity is retained");
    check(id({{"browser_specific_settings", {{"gecko", {{"id", "modern@example.test"}}}}}}) == "modern@example.test", "current Gecko identity is retained");
    check(id({{"key", publicKey}, {"applications", {{"gecko", {{"id", "legacy@example.test"}}}}},
        {"browser_specific_settings", {{"gecko", {{"id", "modern@example.test"}}}}}}) == "modern@example.test", "current Gecko declaration keeps precedence in mixed manifests");
    check(id({{"key", publicKey}, {"applications", json::object()}}) == "melddjfinppjdikinhbgehiennejpfhp", "unrelated browser metadata does not hide Chrome key");
    for (const auto& key : {"", "=", "====", "A===", "AAAA=", "AAAA====", "AA=A", "AA!A", "AAA", " AAAA", "AA_A", "AA-A", "-----BEGIN PUBLIC KEY-----AAAA",
        "-----BEGIN PUBLIC KEY-----AAAA-----END PUBLIC KEY-----garbage", "-----BEGIN PRIVATE KEY-----AAAA-----END PRIVATE KEY-----"})
        rejects({{"key", key}}, "malformed or unsupported key cannot silently get a local identity");
    rejects({{"key", 42}}, "non-string key is rejected");
    rejects({{"key", std::string(1024 * 1024 + 1, 'A')}}, "oversized key is bounded");
    rejects({{"browser_specific_settings", {{"gecko", {{"id", 42}}}}}}, "non-string Gecko identity is rejected");
    rejects({{"applications", {{"gecko", {{"id", "../escape"}}}}}}, "unsafe declared identity is rejected");
    rejects(json::array(), "non-object manifest is rejected");
    std::printf("EXTENSION_IDENTITY_RESULT checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
