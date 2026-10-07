#include "DashManifestHaiku.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <random>
using namespace SummitDASH;
static std::string wrap(const std::string& address, const std::string& attributes = "")
{
    return "<MPD xmlns='urn:mpeg:dash:schema:mpd:2011' mediaPresentationDuration='PT10S' " + attributes
        + "><BaseURL>media/</BaseURL><Period><AdaptationSet mimeType='video/mp4' codecs='avc1.42c01e'>" + address
        + "<Representation id='low' bandwidth='200000' width='320' height='180'/><Representation id='high' bandwidth='800000' width='640' height='360'/>"
        + "</AdaptationSet></Period></MPD>";
}
int main(int argc, char** argv)
{
    std::string error;
    auto parse = [&](const std::string& text) { return parseManifest(text, "https://example.test/video/manifest.mpd", 1767225700, error); };
    if (argc == 2) {
        std::ifstream stream(argv[1]); std::string source {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        auto actual = parse(source); if (!actual) std::cerr << error << '\n';
        assert(actual && actual->duration == 18 && actual->periods.size() == 1 && actual->periods[0].representations.size() == 3);
        const auto& reps = actual->periods[0].representations;
        assert(reps[0].segments.size() == 9 && reps[1].segments.size() == 9 && reps[2].segments.size() == 10);
        assert(reps[0].width == 320 && reps[1].width == 640 && reps[2].audio && reps[2].codecs == "mp4a.40.2");
    }
    auto fixed = wrap("<SegmentTemplate initialization='$RepresentationID$/init.mp4' media='$RepresentationID$/$Number%05d$.m4s' timescale='1000' duration='2000' startNumber='7'/>");
    auto manifest = parse(fixed); assert(manifest && error.empty());
    auto& first = manifest->periods[0].representations[0];
    assert(manifest->duration == 10 && manifest->periods.size() == 1 && first.segments.size() == 5);
    assert(first.initialization.url == "https://example.test/video/media/low/init.mp4");
    assert(first.segments[0].resource.url == "https://example.test/video/media/low/00007.m4s");
    assert(first.segments.back().number == 11 && first.segments.back().start == 8 && first.segments.back().duration == 2);
    assert(manifest->periods[0].representations[1].segments[0].resource.url == "https://example.test/video/media/high/00007.m4s");
    auto timeline = wrap("<SegmentTemplate initialization='init.mp4' media='$$-$Time$.m4s' timescale='1000' presentationTimeOffset='9000'><SegmentTimeline><S t='9000' d='2000' r='1'/><S d='3000' r='-1'/></SegmentTimeline></SegmentTemplate>");
    manifest = parse(timeline); assert(manifest);
    const auto& timed = manifest->periods[0].representations[0].segments;
    assert(timed.size() == 4 && timed[0].start == 0 && timed[2].time == 13000 && timed.back().start == 7);
    assert(timed[0].resource.url == "https://example.test/video/media/$-9000.m4s");
    auto list = wrap("<SegmentList timescale='1' duration='5'><Initialization sourceURL='one.mp4' range='0-99'/><SegmentURL media='one.mp4' mediaRange='100-499'/><SegmentURL media='one.mp4' mediaRange='500-999'/></SegmentList>");
    manifest = parse(list); assert(manifest);
    const auto& listed = manifest->periods[0].representations[0];
    assert(listed.initialization.range->last == 99 && listed.segments.size() == 2 && listed.segments[1].resource.range->first == 500);
    auto live = fixed;
    live.replace(live.find("mediaPresentationDuration='PT10S'"), std::string("mediaPresentationDuration='PT10S'").size(), "type='dynamic' availabilityStartTime='2026-01-01T00:00:00Z' timeShiftBufferDepth='PT10S' minimumUpdatePeriod='PT2S'");
    manifest = parse(live); if(!manifest) std::cerr << live << "\n" << error << "\n"; assert(manifest && manifest->dynamic);
    assert(manifest->liveEdge == 100 && manifest->updatePeriod == 2);
    assert(manifest->periods[0].representations[0].segments.size() == 5);
    assert(manifest->periods[0].representations[0].segments.front().start == 90);
    assert(manifest->periods[0].representations[0].segments.back().start == 98);
    auto periods = "<MPD mediaPresentationDuration='PT10S'><Period start='PT0S' duration='PT4S'><SegmentTemplate initialization='init.mp4' media='$Number$.m4s' duration='2'/><AdaptationSet contentType='audio' codecs='mp4a.40.2'><Representation id='a' bandwidth='96000'/></AdaptationSet></Period><Period start='PT4S'><AdaptationSet mimeType='video/mp4'><SegmentTemplate initialization='init2.mp4' media='$Number$.m4s' duration='2'/><Representation id='v' bandwidth='300000'><SegmentTemplate startNumber='8'/></Representation></AdaptationSet></Period></MPD>";
    manifest = parse(periods); assert(manifest && manifest->periods.size() == 2);
    assert(manifest->periods[0].representations[0].audio && manifest->periods[1].representations[0].segments[0].number == 8);
    assert(manifest->periods[1].representations[0].segments.back().start == 8);
    for (const char* date : {"2026-02-31T00:00:00Z", "2026-01-01T00:00:nanZ", "2026-01-01T00:00:00+-1:00"}) {
        auto invalid = live; auto at = invalid.find("2026-01-01T00:00:00Z");
        invalid.replace(at, 20, date); assert(!parse(invalid));
    }
    for (const std::string& invalid : {
        "<!DOCTYPE MPD [<!ENTITY test 'unsafe'>]>" + fixed,
        wrap("<SegmentTemplate initialization='file:///etc/passwd' media='$Number$.m4s' duration='2'/>") ,
        wrap("<SegmentTemplate initialization='init' media='https://user:password@example.test/a' duration='2'/>") ,
        wrap("<SegmentTemplate initialization='init' media='$Number%09999d$' duration='2'/>") ,
        wrap("<SegmentTemplate initialization='init' media='$Number$' duration='0'/>") ,
        wrap("<SegmentTemplate initialization='init' media='$Number$' duration='1' timescale='9999999999'/>") ,
        wrap("<SegmentTemplate initialization='init' media='$Number$' duration='1' presentationTimeOffset='18446744073709551615'/>") ,
        wrap("<SegmentTemplate initialization='init' media='$Number$'><SegmentTimeline><S d='1' r='18446744073709551615'/></SegmentTimeline></SegmentTemplate>") ,
        wrap("<SegmentList duration='5'><Initialization sourceURL='one' range='99-0'/><SegmentURL media='one'/></SegmentList>")
    }) { assert(!parse(invalid) && !error.empty()); }
    // Boundary fuzzing supplements semantic cases with malformed XML and
    // integer/token substitutions, under ASan/UBSan in the host test command.
    std::mt19937 random(38);
    for (unsigned i = 0; i < 5000; ++i) {
        auto altered = fixed;
        unsigned count = random() % 8 + 1;
        for (unsigned j = 0; j < count; ++j) altered[random() % altered.size()] = char(random() % 127);
        parse(altered);
    }
    std::cout << "DASH_MANIFEST_PASS fixed timeline ranges live rejection fuzz\n";
}
