#include "WebExtensionCRXHaiku.h"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

static unsigned checks, failures;
static void check(bool result, const std::string& message)
{
    ++checks; failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", message.c_str());
}
static std::string digest(std::span<const uint8_t> bytes)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> result; unsigned length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), result.data(), &length, EVP_sha256(), nullptr) != 1 || length != 32) return { };
    std::string text;
    for (unsigned i = 0; i < length; ++i) { text += "0123456789abcdef"[result[i] >> 4]; text += "0123456789abcdef"[result[i] & 15]; }
    return text;
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const auto root = std::filesystem::path(argv[1]);
    std::ifstream expectedFile(root / "expected.json");
    const auto expected = nlohmann::json::parse(expectedFile);
    for (const auto& [name, expectation] : expected.items()) {
        std::ifstream input(root / name, std::ios::binary);
        std::vector<uint8_t> bytes { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
        if (!input || digest(bytes) != expectation.at("sha256").get<std::string>()) { check(false, name + " fixture bytes"); continue; }
        auto result = WebKit::verifyWebExtensionCRXHaiku(bytes);
        check(result.has_value() == expectation.at("valid").get<bool>(), name + " signature/header outcome");
        if (result && expectation.at("valid").get<bool>()) {
            check(result->identifier == expectation.at("identifier").get<std::string>(), name + " verified developer identity");
            check(result->archiveOffset == expectation.at("archive_offset").get<size_t>() && result->archiveOffset <= bytes.size(), name + " exact ZIP boundary");
            if (result->archiveOffset <= bytes.size()) check(digest(std::span<const uint8_t>(bytes).subspan(result->archiveOffset)) == expectation.at("archive_sha256").get<std::string>(), name + " authenticated payload bytes");
        }
    }
    std::printf("CRX_RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
