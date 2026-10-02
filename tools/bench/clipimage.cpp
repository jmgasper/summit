// Puts an image file on the clipboard the way Haiku's Screenshot does: the
// picture as an archived BBitmap under "image/bitmap". For paste tests.
//
//   clipimage FILE        (any format the Translation Kit reads)
//
// Build on Haiku: g++ -O2 -o clipimage clipimage.cpp -lbe -ltranslation
#include <Application.h>
#include <Bitmap.h>
#include <Clipboard.h>
#include <TranslationUtils.h>
#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: clipimage FILE\n");
        return 2;
    }
    BApplication app("application/x-vnd.Summit-ClipImage");
    BBitmap* bitmap = BTranslationUtils::GetBitmap(argv[1]);
    if (!bitmap) {
        std::fprintf(stderr, "clipimage: cannot read %s\n", argv[1]);
        return 1;
    }
    if (!be_clipboard->Lock()) return 1;
    be_clipboard->Clear();
    BMessage archive;
    bitmap->Archive(&archive);
    be_clipboard->Data()->AddMessage("image/bitmap", &archive);
    be_clipboard->Commit();
    be_clipboard->Unlock();
    std::printf("clipboard: %s, %.0fx%.0f\n", argv[1], bitmap->Bounds().Width() + 1, bitmap->Bounds().Height() + 1);
    delete bitmap;
    return 0;
}
