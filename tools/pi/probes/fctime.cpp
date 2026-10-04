// fctime: how long fontconfig takes to initialise and match "sans-serif",
// and whether it finds/writes its cache.
#include <OS.h>
#include <fontconfig/fontconfig.h>
#include <cstdio>
int main()
{
    bigtime_t t0 = system_time();
    FcConfig* config = FcInitLoadConfigAndFonts();
    bigtime_t t1 = system_time();
    FcFontSet* set = FcConfigGetFonts(config, FcSetSystem);
    FcPattern* pattern = FcNameParse((const FcChar8*)"sans-serif");
    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern* match = FcFontMatch(config, pattern, &result);
    bigtime_t t2 = system_time();
    FcChar8* file = nullptr;
    if (match) FcPatternGetString(match, FC_FILE, 0, &file);
    std::printf("init+fonts %.1f ms, match %.1f ms, %d fonts, sans-serif -> %s\n", (t1 - t0) / 1e3, (t2 - t1) / 1e3,
        set ? set->nfont : -1, file ? (const char*)file : "(none)");
    FcStrList* dirs = FcConfigGetCacheDirs(config);
    for (FcChar8* d; (d = FcStrListNext(dirs));) std::printf("cache dir %s\n", d);
    return 0;
}
