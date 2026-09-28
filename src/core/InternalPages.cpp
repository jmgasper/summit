#include "InternalPages.h"
#include "Address.h"
#include "Favicon.h"
#include <cstdio>
#include <ctime>
#include <map>

namespace summit {
static std::tm LocalTime(int64_t seconds)
{
    std::time_t time = static_cast<std::time_t>(seconds);
    std::tm result { };
    localtime_r(&time, &result);
    return result;
}
static int DayKey(const std::tm& time) { return (time.tm_year + 1900) * 1000 + time.tm_yday; }

static std::string DateLabel(const std::tm& time)
{
    static const char* days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char* months[] = { "January", "February", "March", "April", "May", "June", "July",
        "August", "September", "October", "November", "December" };
    return std::string(days[time.tm_wday % 7]) + ", " + std::to_string(time.tm_mday) + " "
        + months[time.tm_mon % 12] + " " + std::to_string(time.tm_year + 1900);
}

std::vector<HistoryDay> GroupHistoryByDay(const std::vector<PageRecord>& history, int64_t now)
{
    const std::tm today = LocalTime(now);
    // Step back a calendar day at noon, so daylight saving changes cannot skip one.
    std::tm previous = today;
    previous.tm_mday -= 1;
    previous.tm_hour = 12;
    previous.tm_min = previous.tm_sec = 0;
    previous.tm_isdst = -1;
    const std::tm yesterday = LocalTime(static_cast<int64_t>(std::mktime(&previous)));
    std::vector<HistoryDay> days;
    std::map<int, size_t> byKey;
    for (const auto& page : history) {
        int key = -1;
        std::string label = "Earlier";
        if (page.visited > 0) {
            const std::tm visited = LocalTime(page.visited);
            key = DayKey(visited);
            label = key == DayKey(today) ? "Today" : key == DayKey(yesterday) ? "Yesterday" : DateLabel(visited);
        }
        auto [found, inserted] = byKey.try_emplace(key, days.size());
        if (inserted) days.push_back({label, { }});
        days[found->second].pages.push_back(&page);
    }
    return days;
}

static const char* kStyle = R"(<style>
*{box-sizing:border-box}[hidden]{display:none!important}
body{margin:0;color:#243f36;font:14px/1.45 system-ui,sans-serif;background:#f9fbf8}
main{max-width:880px;margin:auto;padding:36px 40px 48px}
header{display:flex;align-items:center;gap:16px;margin-bottom:22px}
h1{font-size:30px;letter-spacing:-.5px;margin:0;flex:1}
input{font:inherit;width:280px;padding:7px 11px;border:1px solid #d5e0d6;border-radius:8px;background:white;color:inherit}
input:focus{outline:2px solid #9cc3ac;border-color:#9cc3ac}
section{margin-bottom:22px}
h2{font-size:12px;letter-spacing:1.5px;text-transform:uppercase;color:#71877b;margin:0 0 6px;padding:0 10px}
.list{background:white;border:1px solid #e2e9e1;border-radius:10px;overflow:hidden}
.row{display:flex;align-items:center;gap:12px;padding:8px 12px;color:inherit;text-decoration:none;border-top:1px solid #eef2ed}
.row:first-child{border-top:0}a.row:hover{background:#eef5ed}
.icon{flex:none;width:16px;height:16px;background:center/16px 16px no-repeat}
.letter{border-radius:4px;background:#dfeade;color:#456c51;font-size:10px;font-weight:700;display:grid;place-items:center}
.text{flex:1;min-width:0;display:flex;gap:10px;align-items:baseline}
.title{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:60%}
.url{flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;color:#7b897f;font-size:12px}
.time{flex:none;color:#7b897f;font-size:12px;font-variant-numeric:tabular-nums}
.empty{color:#63766b;padding:28px 12px;text-align:center}
</style>
)";

static const char* kFilterScript = R"(<script>
const filter = document.getElementById('filter');
filter.addEventListener('input', () => {
  const query = filter.value.trim().toLowerCase();
  let any = false;
  for (const section of document.querySelectorAll('section')) {
    let shown = 0;
    for (const row of section.querySelectorAll('.row')) {
      const match = !query || (row.textContent + ' ' + (row.href || '')).toLowerCase().includes(query);
      row.hidden = !match;
      if (match) ++shown;
    }
    section.hidden = !shown;
    any = any || shown > 0;
  }
  const none = document.getElementById('none');
  if (none) none.hidden = any;
});
</script>
)";

namespace {
class PageWriter {
public:
    PageWriter(const IconSource& icons) : fIcons(icons) { }

    void Row(const PageRecord& page, const std::string& trailing)
    {
        const bool link = page.url.rfind("https://", 0) == 0 || page.url.rfind("http://", 0) == 0
            || page.url.rfind("file://", 0) == 0;
        fBody += link ? "<a class=\"row\" href=\"" + EscapeHTML(page.url) + "\">" : std::string("<div class=\"row\">");
        fBody += Icon(page.url);
        auto shown = page.url;
        if (auto scheme = shown.find("://"); scheme != std::string::npos) shown.erase(0, scheme + 3);
        fBody += "<span class=\"text\"><span class=\"title\">" + EscapeHTML(page.title.empty() ? page.url : page.title)
            + "</span><span class=\"url\">" + EscapeHTML(shown) + "</span></span>";
        if (!trailing.empty()) fBody += "<span class=\"time\">" + EscapeHTML(trailing) + "</span>";
        fBody += link ? "</a>\n" : "</div>\n";
    }
    void Raw(const std::string& html) { fBody += html; }

    std::string Finish(const char* title, const char* placeholder, bool empty, const char* emptyText) const
    {
        std::string page = "<!doctype html>\n<html lang=\"en\">\n<meta charset=\"utf-8\">\n<title>";
        page += title;
        page += "</title>\n";
        page += kStyle;
        if (!fIconStyles.empty()) page += "<style>\n" + fIconStyles + "</style>\n";
        page += "<main>\n<header><h1>";
        page += title;
        page += "</h1>";
        if (!empty) page += std::string("<input id=\"filter\" type=\"search\" placeholder=\"") + placeholder + "\" autofocus>";
        page += "</header>\n";
        if (empty) page += std::string("<p class=\"empty\">") + emptyText + "</p>\n";
        else {
            page += fBody;
            page += "<p class=\"empty\" id=\"none\" hidden>Nothing matches your search.</p>\n";
        }
        page += "</main>\n";
        if (!empty) page += kFilterScript;
        page += "</html>\n";
        return page;
    }

private:
    std::string Icon(const std::string& url)
    {
        std::string image = fIcons ? fIcons(url) : std::string();
        // Only data: images are emitted, so a page cannot be made to load anything.
        if (image.rfind("data:image/", 0) == 0 && image.find_first_of("\"'()\\<>") == std::string::npos) {
            auto [found, inserted] = fIconClasses.try_emplace(image, fIconClasses.size());
            if (inserted)
                fIconStyles += ".i" + std::to_string(found->second) + "{background-image:url(" + image + ")}\n";
            return "<span class=\"icon i" + std::to_string(found->second) + "\"></span>";
        }
        auto host = FaviconKey(url);
        if (host.rfind("www.", 0) == 0) host.erase(0, 4);
        char letter = '*';
        for (char c : host) if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { letter = c; break; }
        if (letter >= 'a' && letter <= 'z') letter = char(letter - 'a' + 'A');
        return std::string("<span class=\"icon letter\">") + letter + "</span>";
    }

    const IconSource& fIcons;
    std::map<std::string, size_t> fIconClasses;
    std::string fIconStyles;
    std::string fBody;
};
}

std::string RenderHistoryPage(const std::vector<PageRecord>& history, int64_t now, const IconSource& icons)
{
    PageWriter writer(icons);
    for (const auto& day : GroupHistoryByDay(history, now)) {
        writer.Raw("<section><h2>" + EscapeHTML(day.label) + "</h2><div class=\"list\">\n");
        for (const auto* page : day.pages) {
            std::string time;
            if (page->visited > 0) {
                const std::tm visited = LocalTime(page->visited);
                char text[8];
                std::snprintf(text, sizeof text, "%02d:%02d", visited.tm_hour, visited.tm_min);
                time = text;
            }
            writer.Row(*page, time);
        }
        writer.Raw("</div></section>\n");
    }
    return writer.Finish("History", "Search History", history.empty(), "Pages you visit will be listed here.");
}

std::string RenderBookmarksPage(const std::vector<PageRecord>& bookmarks, const IconSource& icons)
{
    PageWriter writer(icons);
    for (bool bar : {true, false}) {
        bool any = false;
        for (const auto& page : bookmarks) any = any || page.bar == bar;
        if (!any) continue;
        writer.Raw(std::string("<section><h2>") + (bar ? "Bookmarks Bar" : "Other Bookmarks") + "</h2><div class=\"list\">\n");
        for (const auto& page : bookmarks) if (page.bar == bar) writer.Row(page, { });
        writer.Raw("</div></section>\n");
    }
    return writer.Finish("Bookmarks", "Search Bookmarks", bookmarks.empty(),
        "No bookmarks yet. Use Bookmarks › Bookmark This Page, or the star in the toolbar.");
}

std::string RenderPrivateStartPage(const std::string& searchEngineName, const std::string& searchPrefix)
{
    // Every engine takes the query as "q"; the form goes to the address before it.
    const auto action = searchPrefix.substr(0, searchPrefix.find('?'));
    std::string page = R"(<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>New Private Tab</title>
<style>
*{box-sizing:border-box}html{background:#2a1a45}body{margin:0;color:#f1ebfb;font:16px/1.55 system-ui,sans-serif;
background:linear-gradient(160deg,#3b2566 0%,#2a1a45 55%,#20133a 100%);min-height:100vh}
main{max-width:720px;margin:auto;padding:64px 40px 40px}
.mask{width:72px;height:36px;border-radius:18px;background:#fff;position:relative;margin-bottom:26px}
.mask:before,.mask:after{content:"";position:absolute;top:11px;width:18px;height:13px;border-radius:50%;background:#3b2566}
.mask:before{left:13px}.mask:after{right:13px}
h1{font-size:38px;line-height:1.15;margin:0 0 14px;letter-spacing:-.5px}
p{margin:0 0 12px;color:#d6caec}
form{display:flex;margin:30px 0 34px;background:#fff;border-radius:10px;overflow:hidden;box-shadow:0 6px 24px rgba(0,0,0,.28)}
input{flex:1;border:0;padding:14px 16px;font:inherit;color:#1e1530;outline:none;min-width:0}
button{border:0;background:#8460c4;color:#fff;font:inherit;font-weight:600;padding:0 20px;cursor:pointer}
button:hover{background:#9670d6}
.cards{display:grid;grid-template-columns:1fr 1fr;gap:16px}
.card{background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.12);border-radius:10px;padding:16px 18px}
.card h2{font-size:14px;margin:0 0 6px;color:#fff;letter-spacing:.3px}.card p{font-size:14px;margin:0}
.note{margin-top:26px;font-size:13px;color:#b7a8d4}
@media(max-width:620px){main{padding:40px 22px}.cards{grid-template-columns:1fr}h1{font-size:30px}}
</style>
<main>
<div class="mask" aria-hidden="true"></div>
<h1>Private Browsing</h1>
<p>Pages you open in private windows are not added to your history. Their cookies, site data and cache
are kept apart from your other windows and forgotten when you close the last private window.</p>
<form action=")" + EscapeHTML(action) + R"(" method="get">
<input name="q" type="search" placeholder="Search with )" + EscapeHTML(searchEngineName) + R"(" aria-label="Search" autofocus>
<button type="submit">Search</button>
</form>
<div class="cards">
<div class="card"><h2>SUMMIT FORGETS</h2><p>The pages you visit, what you search for, cookies, site data, cached files and zoom levels you set here.</p></div>
<div class="card"><h2>SUMMIT KEEPS</h2><p>Files you download and bookmarks you create. Extensions do not run in private windows.</p></div>
</div>
<p class="note">Private browsing does not hide you from the websites you visit, your employer or school, or your internet provider.</p>
</main>
</html>
)";
    return page;
}
}
