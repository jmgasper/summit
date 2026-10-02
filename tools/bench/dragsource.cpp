// A drag source for drop tests: a small window that, when pressed, drags the
// files named on the command line the way Tracker does (B_SIMPLE_DATA with
// "refs"). Drive the pointer from the window to the drop target over VNC
// (tools/bench/vnc-input.py drag); no keyboard is needed.
//
//   dragsource X Y FILE...      the window's top-left corner on screen
//
// Build on Haiku: g++ -O2 -o dragsource dragsource.cpp -lbe
#include <Application.h>
#include <Entry.h>
#include <String.h>
#include <View.h>
#include <Window.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

class SourceView : public BView {
public:
    SourceView(BRect frame, std::vector<entry_ref> refs)
        : BView(frame, "source", B_FOLLOW_ALL, B_WILL_DRAW), fRefs(std::move(refs))
    {
        SetViewColor(80, 160, 230);
    }
    void Draw(BRect) override
    {
        SetHighColor(255, 255, 255);
        SetLowColor(ViewColor());
        BString label;
        label << "Drag " << (int32)fRefs.size() << (fRefs.size() == 1 ? " file" : " files");
        DrawString(label.String(), BPoint(10, 24));
    }
    void MouseDown(BPoint where) override
    {
        BMessage drag(B_SIMPLE_DATA);
        for (const auto& ref : fRefs) drag.AddRef("refs", &ref);
        drag.AddInt32("be:actions", B_COPY_TARGET);
        BRect rect(where.x - 16, where.y - 16, where.x + 16, where.y + 16);
        DragMessage(&drag, rect);
        std::printf("drag started with %zu files\n", fRefs.size());
        std::fflush(stdout);
    }

private:
    std::vector<entry_ref> fRefs;
};

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: dragsource X Y FILE...\n");
        return 2;
    }
    BApplication app("application/x-vnd.Summit-DragSource");
    std::vector<entry_ref> refs;
    for (int i = 3; i < argc; ++i) {
        entry_ref ref;
        if (get_ref_for_path(argv[i], &ref) == B_OK) refs.push_back(ref);
        else std::fprintf(stderr, "dragsource: no such file: %s\n", argv[i]);
    }
    const float x = std::atof(argv[1]), y = std::atof(argv[2]);
    auto* window = new BWindow(BRect(x, y, x + 160, y + 60), "Drag source", B_FLOATING_WINDOW_LOOK,
        B_FLOATING_ALL_WINDOW_FEEL, B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AVOID_FOCUS | B_WILL_ACCEPT_FIRST_CLICK);
    window->AddChild(new SourceView(window->Bounds(), refs));
    window->Show();
    app.Run();
    return 0;
}
