// Native tests of the real upstream configuration class. These need a
// compatible Modern WebKit library; compiling them alone is not a test pass.
#include "config.h"
#include "WebExtensionControllerConfiguration.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include <Application.h>
#include <FindDirectory.h>
#include <Path.h>
#include <cstdio>
#include <filesystem>
#include <sys/stat.h>
#include <wtf/FileSystem.h>

namespace {

unsigned checks;
unsigned failures;

void check(bool condition, const char* description)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

void checkCopy(WebKit::WebExtensionControllerConfiguration& original)
{
    auto copy = original.copy();
    check(copy.ptr() != &original, "copy has distinct object identity");
    check(copy->identifier() == original.identifier(), "copy preserves identifier");
    check(copy->storageIsPersistent() == original.storageIsPersistent(), "copy preserves persistence");
    check(copy->storageIsTemporary() == original.storageIsTemporary(), "copy preserves temporary state");
    check(copy->storageDirectory() == original.storageDirectory(), "copy preserves storage path");
    check(&copy->defaultWebsiteDataStore() == &original.defaultWebsiteDataStore(), "copy shares website data store");
    check(copy.get() == original, "native copy compares equal");
}

} // namespace

int main()
{
    BApplication application("application/x-vnd.Summit-ExtensionConfigurationTests");
    using Configuration = WebKit::WebExtensionControllerConfiguration;

    // Initialization and copying cases adapted from upstream
    // Tools/TestWebKitAPI/Tests/WebKit/WKWebView/WKWebExtensionControllerConfiguration.mm.
    auto defaults = Configuration::createDefault();
    check(defaults->storageIsPersistent(), "default configuration is persistent");
    check(!defaults->storageIsTemporary() && !defaults->identifier(), "default has no temporary state or UUID");
    check(defaults->defaultWebsiteDataStore().isPersistent(), "default website data store is persistent");
    check(&defaults->defaultWebsiteDataStore() == &WebKit::WebsiteDataStore::defaultDataStore(), "default uses shared website data store");
    BPath settings;
    check(find_directory(B_USER_SETTINGS_DIRECTORY, &settings) == B_OK, "settings directory is available");
    auto applicationRoot = FileSystem::pathByAppendingComponent(String::fromUTF8(settings.Path()), "WebKit"_s);
    applicationRoot = FileSystem::pathByAppendingComponent(applicationRoot, FileSystem::encodeForFileName(String::fromUTF8(application.Signature())));
    applicationRoot = FileSystem::pathByAppendingComponent(applicationRoot, "WebExtensions"_s);
    check(defaults->storageDirectory() == FileSystem::pathByAppendingComponent(applicationRoot, "Default"_s), "default uses native application profile root");
    checkCopy(defaults.get());

    auto ephemeral = Configuration::createNonPersistent();
    check(!ephemeral->storageIsPersistent() && ephemeral->storageDirectory().isNull(), "nonpersistent configuration has no storage path");
    check(!ephemeral->storageIsTemporary() && !ephemeral->identifier(), "nonpersistent has no temporary state or UUID");
    check(!ephemeral->defaultWebsiteDataStore().isPersistent(), "nonpersistent factory creates an ephemeral website data store");
    auto otherEphemeral = Configuration::createNonPersistent();
    check(&ephemeral->defaultWebsiteDataStore() != &otherEphemeral->defaultWebsiteDataStore(), "separate nonpersistent factories isolate website data stores");
    check(!(ephemeral.get() == otherEphemeral.get()), "distinct ephemeral website data stores compare unequal");
    checkCopy(ephemeral.get());

    auto identifier = WTF::UUID::createVersion4();
    auto identified = Configuration::create(identifier);
    check(identified->identifier() == identifier, "UUID factory preserves identifier");
    check(identified->storageIsPersistent() && !identified->storageIsTemporary(), "UUID configuration is persistent");
    check(identified->storageDirectory() == FileSystem::pathByAppendingComponent(applicationRoot, identifier.toString().convertToASCIIUppercase()), "UUID path uses uppercase UUID");
    check(!FileSystem::fileExists(identified->storageDirectory()), "UUID factory does not eagerly create persistent directories");
    auto sameIdentifier = Configuration::create(identifier);
    check(identified.get() == sameIdentifier.get(), "same UUID and default store compare equal");
    checkCopy(identified.get());
    identified->setStorageDirectory("/boot/home/config/settings/Summit/Test Profile/WebExtensions"_s);
    identified->setDefaultWebsiteDataStore(&ephemeral->defaultWebsiteDataStore());
    checkCopy(identified.get());
    check(!(identified.get() == sameIdentifier.get()), "custom storage and data store affect equality");

    String temporaryPath;
    RefPtr<Configuration> temporaryCopy;
    {
        auto temporary = Configuration::createTemporary();
        temporaryPath = temporary->storageDirectory();
        check(temporary->storageIsPersistent() && temporary->storageIsTemporary(), "temporary configuration uses temporary disk storage");
        check(!temporary->identifier(), "temporary configuration has no UUID");
        std::error_code error;
        auto nativeTemporary = std::filesystem::temp_directory_path(error).string();
        check(!error, "native temporary parent is available");
        check(FileSystem::parentPath(temporaryPath) == String::fromUTF8(nativeTemporary.c_str()), "temporary configuration is inside native temporary directory");
        struct stat status { };
        check(!stat(temporaryPath.utf8().legacyCStringPointer(), &status)
            && S_ISDIR(status.st_mode) && (status.st_mode & 0777) == 0700, "temporary directory exists with private permissions");
        checkCopy(temporary.get());
        temporaryCopy = temporary->copy();
        auto secondTemporary = Configuration::createTemporary();
        check(secondTemporary->storageDirectory() != temporaryPath, "temporary factories create distinct directories");
        check(FileSystem::deleteNonEmptyDirectory(secondTemporary->storageDirectory()), "test cleans second temporary directory");
    }
    check(FileSystem::fileExists(temporaryPath), "temporary storage survives original configuration destruction");
    check(temporaryCopy->storageDirectory() == temporaryPath, "surviving copy retains shared temporary path");
    temporaryCopy = nullptr;
    // Upstream configuration does not own deletion: serialized configurations
    // can reopen this directory. Clean it explicitly only after all users end.
    check(FileSystem::fileExists(temporaryPath), "temporary storage retains upstream lifetime after all copies end");
    check(FileSystem::deleteNonEmptyDirectory(temporaryPath), "test cleans shared temporary directory");

    std::printf("CONFIGURATION_RESULT %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}

#endif
