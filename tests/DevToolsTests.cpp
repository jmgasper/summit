#include "core/DevTools.h"
#include "core/DevToolsFormat.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " #x "\n"; } } while (0)
#define CHECK_TEXT(actual, expected) do { ++checks; const std::string a = (actual), e = (expected); if (a != e) { ++failures; \
    std::cerr << __FILE__ << ':' << __LINE__ << ": expected\n" << e << "\n---- got\n" << a << "\n----\n"; } } while (0)

using namespace summit::devtools;

static void TestKinds()
{
    CHECK(DetectBodyKind("application/json", "") == BodyKind::JSON);
    CHECK(DetectBodyKind("Application/JSON; charset=utf-8", "") == BodyKind::JSON);
    CHECK(DetectBodyKind("application/vnd.api+json", "") == BodyKind::JSON);
    CHECK(DetectBodyKind("text/html; charset=UTF-8", "") == BodyKind::HTML);
    CHECK(DetectBodyKind("application/xhtml+xml", "") == BodyKind::HTML);
    CHECK(DetectBodyKind("text/xml", "") == BodyKind::XML);
    CHECK(DetectBodyKind("application/rss+xml", "") == BodyKind::XML);
    CHECK(DetectBodyKind("image/svg+xml", "") == BodyKind::XML);
    CHECK(DetectBodyKind("image/png", "") == BodyKind::Image);
    CHECK(DetectBodyKind("text/javascript", "") == BodyKind::JavaScript);
    CHECK(DetectBodyKind("text/css", "") == BodyKind::CSS);
    CHECK(DetectBodyKind("font/woff2", "") == BodyKind::Binary);
    CHECK(DetectBodyKind("text/plain", "hello") == BodyKind::Text);
    // What the type does not say, the body may.
    CHECK(DetectBodyKind("text/plain", " {\"a\": [1, 2]}\n") == BodyKind::JSON);
    CHECK(DetectBodyKind("text/plain", "{not json}") == BodyKind::Text);
    CHECK(DetectBodyKind("", "[1,2,3]") == BodyKind::JSON);
    CHECK(DetectBodyKind("", "<?xml version=\"1.0\"?><a/>") == BodyKind::XML);
    CHECK(DetectBodyKind("application/octet-stream", "<!DOCTYPE HTML><html></html>") == BodyKind::HTML);
    CHECK(DetectBodyKind("application/octet-stream", std::string("\x89PNG\0\0", 6)) == BodyKind::Binary);
    CHECK(DetectBodyKind("text/plain", std::string("a\0b", 3)) == BodyKind::Text);
    CHECK(std::string(BodyKindLexer(BodyKind::HTML)) == "hypertext" && std::string(BodyKindLexer(BodyKind::Image)) == "null");
    CHECK(CanFormat(BodyKind::JSON) && CanFormat(BodyKind::XML) && CanFormat(BodyKind::HTML) && !CanFormat(BodyKind::CSS));
}

static void TestJSON()
{
    std::string out;
    CHECK(FormatJSON("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{},\"d\":[ ]}", out));
    CHECK_TEXT(out, "{\n  \"a\": 1,\n  \"b\": [\n    true,\n    null,\n    \"x\"\n  ],\n  \"c\": {},\n  \"d\": []\n}");
    // Tokens are copied: no number or escape is rewritten.
    CHECK(FormatJSON("[12345678901234567890123, 1.0, 1e3, -0, \"\\u00e9\\n\\\"{[,:]}\"]", out));
    CHECK_TEXT(out, "[\n  12345678901234567890123,\n  1.0,\n  1e3,\n  -0,\n  \"\\u00e9\\n\\\"{[,:]}\"\n]");
    CHECK(FormatJSON("  \"text\" ", out) && out == "\"text\"");
    CHECK(FormatJSON("\xEF\xBB\xBF[\r\n1\r\n]", out) && out == "[\n  1\n]");
    CHECK(FormatJSON(")]}'\n{\"a\":1}", out));
    CHECK_TEXT(out, ")]}'\n{\n  \"a\": 1\n}");
    CHECK(FormatJSON("while(1);[1]", out) && out == "while(1);\n[\n  1\n]");
    out = "kept";
    CHECK(!FormatJSON("{\"a\":1,}", out) && out == "kept");
    CHECK(!FormatJSON("", out) && !FormatJSON("{", out) && !FormatJSON("[1] [2]", out) && !FormatJSON("{'a':1}", out));
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += '[';
    for (int i = 0; i < 200; ++i) deep += ']';
    CHECK(FormatJSON(deep, out) && out.find("[]") != std::string::npos);
    const auto result = FormatBody(BodyKind::JSON, "{broken");
    CHECK(!result.formatted && result.text == "{broken" && !result.note.empty());
    CHECK(FormatBody(BodyKind::CSS, "a{}").text == "a{}" && !FormatBody(BodyKind::CSS, "a{}").formatted);
}

static void TestXML()
{
    CHECK_TEXT(FormatXML("<?xml version=\"1.0\"?><root><item id=\"1\">One</item><item id=\"2\"/><empty></empty>"
        "<group><a>1</a><!-- note --><b><![CDATA[x < y]]></b></group></root>"),
        "<?xml version=\"1.0\"?>\n<root>\n  <item id=\"1\">One</item>\n  <item id=\"2\"/>\n  <empty></empty>\n"
        "  <group>\n    <a>1</a>\n    <!-- note -->\n    <b><![CDATA[x < y]]></b>\n  </group>\n</root>");
    // Formatting what is formatted already changes nothing.
    const std::string formatted = "<a>\n  <b>text</b>\n  <c>\n    <d/>\n  </c>\n</a>";
    CHECK_TEXT(FormatXML(formatted), formatted);
    CHECK_TEXT(FormatXML("<a x=\"1 > 0\"\n   y='2'><b>two\nlines</b></a>"),
        "<a x=\"1 > 0\" y='2'>\n  <b>\n    two\n    lines\n  </b>\n</a>");
    CHECK_TEXT(FormatXML("<!DOCTYPE r [ <!ENTITY e \"v\"> ]><r>text <i>mixed</i> more</r>"),
        "<!DOCTYPE r [ <!ENTITY e \"v\"> ]>\n<r>\n  text\n  <i>mixed</i>\n  more\n</r>");
    // Not well formed: laid out all the same, nothing lost.
    CHECK_TEXT(FormatXML("</a><b>1 < 2</b><c"), "</a>\n<b>1 < 2</b>\n<c");
    CHECK_TEXT(FormatXML("plain"), "plain");
    CHECK(FormatXML("").empty());
    CHECK_TEXT(FormatXML("<Case><case>x</case></Case>"), "<Case>\n  <case>x</case>\n</Case>");
}

static void TestHTML()
{
    CHECK_TEXT(FormatHTML("<!DOCTYPE html><html><head><title>A page</title><meta charset=utf-8>"
        "<link rel=stylesheet href=a.css></head><body><h1>Title</h1><p>Some <b>bold</b> and <a href=\"/x\">linked</a> text."
        "<p>Second<br>line</p><ul><li>One<li>Two</ul><img src=a.png></body></html>"),
        "<!DOCTYPE html>\n<html>\n  <head>\n    <title>A page</title>\n    <meta charset=utf-8>\n"
        "    <link rel=stylesheet href=a.css>\n  </head>\n  <body>\n    <h1>Title</h1>\n    <p>\n"
        "      Some <b>bold</b> and <a href=\"/x\">linked</a> text.\n    <p>\n      Second<br>\n      line\n    </p>\n"
        "    <ul>\n      <li>\n        One\n      <li>\n        Two\n    </ul>\n    <img src=a.png>\n  </body>\n</html>");
    // Scripts and styles keep their text; only the indentation they share changes.
    CHECK_TEXT(FormatHTML("<div><script>\n        if (a < b) {\n            go('</div>');\n        }\n    </script>"
        "<style>p{color:red}</style></div>"),
        "<div>\n  <script>\n    if (a < b) {\n        go('</div>');\n    }\n  </script>\n  <style>\n    p{color:red}\n  </style>\n</div>");
    CHECK_TEXT(FormatHTML("<DIV CLASS=a><PRE>  keep\n   this </PRE><textarea>\n x </textarea></DIV>"),
        "<DIV CLASS=a>\n  <PRE>  keep\n   this </PRE>\n  <textarea>\n x </textarea>\n</DIV>");
    CHECK_TEXT(FormatHTML("<table><tr><td>1<td>2<tr><td>3</table>"),
        "<table>\n  <tr>\n    <td>\n      1\n    <td>\n      2\n  <tr>\n    <td>\n      3\n</table>");
    CHECK_TEXT(FormatHTML("<div><div><span>a</span> <span>b</span></div><!-- c --></div>"),
        "<div>\n  <div><span>a</span> <span>b</span></div>\n  <!-- c -->\n</div>");
    CHECK_TEXT(FormatHTML("<p>a < b and c > d</p>"), "<p>a < b and c > d</p>");
    CHECK_TEXT(FormatHTML("</div><p>text"), "</div>\n<p>\n  text");
    CHECK_TEXT(FormatHTML("<svg viewBox=\"0 0 1 1\"><path d=\"M0 0\"/><g><circle r=\"1\"/></g></svg>"),
        "<svg viewBox=\"0 0 1 1\">\n  <path d=\"M0 0\"/>\n  <g>\n    <circle r=\"1\"/>\n  </g>\n</svg>");
    CHECK_TEXT(FormatHTML("<select><option>a<option>b</select>"),
        "<select>\n  <option>\n    a\n  <option>\n    b\n</select>");
    CHECK_TEXT(FormatHTML("just text\n  over lines"), "just text over lines");
    CHECK(FormatHTML("").empty());
    // An element too long for a line is opened out.
    const std::string words(150, 'w');
    CHECK_TEXT(FormatHTML("<p>" + words + "</p>"), "<p>\n  " + words + "\n</p>");
    std::string nested;
    for (int i = 0; i < 100; ++i) nested += "<div>";
    nested += "x";
    CHECK(FormatHTML(nested).find(std::string(80, ' ') + "x") != std::string::npos);
    CHECK(FormatHTML(nested).find(std::string(81, ' ')) == std::string::npos);
}

static void TestHelpers()
{
    CHECK(FormatBytes(0) == "0 B" && FormatBytes(1023) == "1023 B" && FormatBytes(1536) == "1.5 KB");
    CHECK(FormatBytes(5 * 1024 * 1024) == "5.00 MB" && FormatBytes(3ull * 1024 * 1024 * 1024) == "3.00 GB");
    CHECK(FormatDuration(0.0123) == "12 ms" && FormatDuration(1.234) == "1.23 s" && FormatDuration(150) == "2.5 min");
    CHECK(FormatDuration(-1).empty() && FormatDuration(0.9996) == "1.00 s");
    std::string out;
    CHECK(DecodeBase64("aGVsbG8=", out) && out == "hello");
    CHECK(DecodeBase64("aGVs\nbG8h", out) && out == "hello!");
    CHECK(DecodeBase64("YQ==", out) && out == "a" && DecodeBase64("YQ", out) && out == "a");
    CHECK(DecodeBase64("", out) && out.empty());
    CHECK(DecodeBase64("_-8=", out) && out == "\xff\xef");
    CHECK(!DecodeBase64("a", out) && !DecodeBase64("a$==", out) && !DecodeBase64("YQ==YQ", out));
    CHECK_TEXT(HexDump(std::string("\x00\x01" "ABCDEFGHIJKLMNOP", 18)),
        "00000000  00 01 41 42 43 44 45 46  47 48 49 4a 4b 4c 4d 4e  ..ABCDEFGHIJKLMN\n"
        "00000010  4f 50                                             OP");
    CHECK(HexDump(std::string(100, 'a'), 32).find("… 68 B more") != std::string::npos);
    CHECK(HexDump("").empty());
    CHECK(ValidUTF8("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80") == "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
    CHECK(ValidUTF8("a\xFF" "b") == "a\xEF\xBF\xBD" "b");
    CHECK(ValidUTF8(std::string("a\0b", 3)) == "a\xEF\xBF\xBD" "b");
    CHECK(ValidUTF8("\xC3") == "\xEF\xBF\xBD" && ValidUTF8("\xED\xA0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(ValidUTF8("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");
}

using nlohmann::json;

// A message of a page's agents as the engine sends it: inside one of its target.
static std::string FromTarget(const std::string& target, const json& message)
{
    return json{{"method", "Target.dispatchMessageFromTarget"},
        {"params", {{"targetId", target}, {"message", message.dump()}}}}.dump();
}
static std::string Event(const std::string& target, const std::string& method, const json& parameters)
{
    return FromTarget(target, json{{"method", method}, {"params", parameters}});
}
static std::string TargetCreated(const std::string& target, bool provisional = false, bool paused = false)
{
    json info = {{"targetId", target}, {"type", "page"}};
    if (provisional) info["isProvisional"] = true;
    if (paused) info["isPaused"] = true;
    return json{{"method", "Target.targetCreated"}, {"params", {{"targetInfo", info}}}}.dump();
}
// The commands sent to a target since the last call: (id, method, parameters).
struct Command { uint64_t id; std::string method; json parameters; std::string target; };
static std::vector<Command> Sent(Session& session)
{
    std::vector<Command> commands;
    for (const auto& text : session.TakeOutgoing()) {
        const auto outer = json::parse(text);
        if (outer["method"] != "Target.sendMessageToTarget") {
            commands.push_back({outer["id"], outer["method"], outer["params"], ""});
            continue;
        }
        const auto inner = json::parse(outer["params"]["message"].get<std::string>());
        commands.push_back({inner["id"], inner["method"], inner["params"], outer["params"]["targetId"]});
    }
    return commands;
}
static const Command* Find(const std::vector<Command>& commands, const std::string& method)
{
    for (const auto& command : commands) if (command.method == method) return &command;
    return nullptr;
}
static void Ask(Session& session, const std::string& target, const std::string& id, const std::string& url,
    const std::string& loader, const std::string& type, double time, const json& more = json::object())
{
    json parameters = {{"requestId", id}, {"frameId", "f1"}, {"loaderId", loader}, {"documentURL", "https://example.com/"},
        {"request", {{"url", url}, {"method", "GET"}, {"headers", {{"Accept", "*/*"}}}}}, {"timestamp", time},
        {"walltime", 1790000000.5}, {"initiator", {{"type", "other"}}}, {"type", type}};
    parameters.update(more);
    session.Receive(Event(target, "Network.requestWillBeSent", parameters));
}
static void Respond(Session& session, const std::string& target, const std::string& id, const std::string& loader,
    const std::string& type, int status, const std::string& mime, double time)
{
    session.Receive(Event(target, "Network.responseReceived", {{"requestId", id}, {"frameId", "f1"}, {"loaderId", loader},
        {"timestamp", time}, {"type", type}, {"response", {{"url", "ignored"}, {"status", status}, {"statusText", status == 200 ? "OK" : "Not Found"},
            {"headers", {{"content-type", mime}, {"Cache-Control", "no-store"}}}, {"mimeType", mime}, {"source", "network"},
            {"requestHeaders", {{"User-Agent", "Summit"}, {"Accept", "*/*"}, {"cookie", "a=b"}}}}}}));
}
static void Finish(Session& session, const std::string& target, const std::string& id, double time, uint64_t bytes)
{
    session.Receive(Event(target, "Network.dataReceived", {{"requestId", id}, {"timestamp", time}, {"dataLength", bytes}, {"encodedDataLength", bytes / 2}}));
    session.Receive(Event(target, "Network.loadingFinished", {{"requestId", id}, {"timestamp", time},
        {"metrics", {{"protocol", "h2"}, {"remoteAddress", "93.184.216.34:443"}, {"responseBodyBytesReceived", bytes / 2},
            {"responseHeaderBytesReceived", 100}, {"responseBodyDecodedSize", bytes}}}}));
}

static void TestSession()
{
    Session session;
    session.Start();
    auto commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Target.setPauseOnStart" && commands[0].parameters["pauseOnStart"] == true);
    session.Receive(TargetCreated("page-1"));
    commands = Sent(session);
    CHECK(Find(commands, "Network.enable") && Find(commands, "Console.enable") && Find(commands, "Page.enable"));
    CHECK(Find(commands, "Network.enable")->target == "page-1" && !Find(commands, "Target.resume"));
    CHECK(!session.TakeChanges().Any());

    // A document, a script and a request a script made.
    Ask(session, "page-1", "0.1", "https://example.com/", "L1", "Document", 10);
    Respond(session, "page-1", "0.1", "L1", "Document", 200, "text/html", 10.1);
    Finish(session, "page-1", "0.1", 10.2, 4000);
    Ask(session, "page-1", "0.2", "https://example.com/js/app.js?v=3#x", "L1", "Script", 10.3,
        {{"initiator", {{"type", "parser"}, {"url", "https://example.com/"}, {"lineNumber", 12}}}});
    Ask(session, "page-1", "0.3", "https://api.example.com/v1/items/", "L1", "Fetch", 10.4,
        {{"request", {{"url", "https://api.example.com/v1/items/"}, {"method", "POST"}, {"headers", {{"Content-Type", "application/json"}}},
            {"postData", "{\"q\":1}"}}},
         {"initiator", {{"type", "script"}, {"stackTrace", {{"callFrames", {{{"functionName", "load"}, {"url", "https://example.com/js/app.js"},
            {"lineNumber", 40}, {"columnNumber", 9}, {"scriptId", "1"}}}}}}}}});
    auto changes = session.TakeChanges();
    CHECK(!changes.requestsReset && changes.requests.size() == 3);
    CHECK(session.Requests().size() == 3);
    const auto& document = session.Requests()[0];
    CHECK(document.Name() == "example.com" && document.Host() == "example.com" && document.StatusLabel() == "200");
    CHECK(document.finished && !document.Pending() && !document.IsError() && document.size == 4000 && document.transferred == 2100);
    CHECK(document.protocol == "h2" && document.remoteAddress == "93.184.216.34:443" && document.mimeType == "text/html");
    CHECK(std::abs(document.Duration() - 0.2) < 1e-9);
    // Headers are in the order of their names; the request's are what was sent.
    CHECK(document.requestHeaders.size() == 3 && document.requestHeaders[0].name == "Accept"
        && document.requestHeaders[1].name == "cookie" && document.requestHeaders[2].name == "User-Agent");
    CHECK(document.responseHeaders.size() == 2 && document.responseHeaders[0].name == "Cache-Control");
    const auto& script = session.Requests()[1];
    CHECK(script.Name() == "app.js?v=3" && script.StatusLabel() == "(pending)" && script.Pending() && script.initiator == "example.com:12");
    const auto& fetch = session.Requests()[2];
    CHECK(fetch.Name() == "items" && fetch.Host() == "api.example.com" && fetch.method == "POST");
    CHECK(fetch.hasRequestBody && fetch.requestBody == "{\"q\":1}" && fetch.initiator == "app.js:40");

    // The document's body was asked for when it was complete.
    commands = Sent(session);
    const auto* body = Find(commands, "Network.getResponseBody");
    CHECK(body && body->parameters["requestId"] == "0.1" && body->target == "page-1");
    CHECK(session.Requests()[0].bodyState == BodyState::Waiting);
    session.Receive(FromTarget("page-1", {{"id", body->id}, {"result", {{"body", "<html></html>"}, {"base64Encoded", false}}}}));
    changes = session.TakeChanges();
    CHECK(changes.bodies.size() == 1 && changes.bodies[0] == document.serial);
    CHECK(document.bodyState == BodyState::Loaded && document.body == "<html></html>");
    session.RequestBody(document.serial);
    CHECK(Sent(session).empty());

    // An image's is asked for when it is wanted, and may come in base64.
    Ask(session, "page-1", "0.4", "https://example.com/a.png", "L1", "Image", 10.5);
    Respond(session, "page-1", "0.4", "L1", "Image", 200, "image/png", 10.6);
    Finish(session, "page-1", "0.4", 10.7, 10);
    CHECK(Sent(session).empty());
    const auto image = session.Requests()[3].serial;
    session.RequestBody(image);
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Network.getResponseBody");
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"result", {{"body", "iVBORw=="}, {"base64Encoded", true}}}}));
    CHECK(session.FindRequest(image)->body == "\x89PNG" && session.FindRequest(image)->bodyState == BodyState::Loaded);

    // Failures.
    Respond(session, "page-1", "0.3", "L1", "Fetch", 404, "application/json", 10.8);
    Finish(session, "page-1", "0.3", 10.9, 20);
    commands = Sent(session);
    CHECK(commands.size() == 1);
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"error", {{"code", -32000}, {"message", "No resource with given identifier found"}}}}));
    CHECK(session.Requests()[2].bodyState == BodyState::Failed && session.Requests()[2].bodyError == "No resource with given identifier found");
    CHECK(session.Requests()[2].IsError() && session.Requests()[2].StatusLabel() == "404");
    session.Receive(Event("page-1", "Network.loadingFailed", {{"requestId", "0.2"}, {"timestamp", 11}, {"errorText", "Load cancelled"}, {"canceled", true}}));
    CHECK(script.failed && script.canceled && script.StatusLabel() == "(canceled)" && !script.IsError() && !script.Pending());
    session.TakeChanges();

    // A redirect ends one entry and starts the next.
    Ask(session, "page-1", "0.5", "http://example.com/old", "L1", "XHR", 12);
    Ask(session, "page-1", "0.5", "https://example.com/new", "L1", "", 12.1,
        {{"redirectResponse", {{"url", "http://example.com/old"}, {"status", 301}, {"statusText", "Moved Permanently"},
            {"headers", {{"Location", "https://example.com/new"}}}, {"mimeType", "text/html"}, {"source", "network"}}}, {"type", nullptr}});
    CHECK(session.Requests().size() == 6);
    const auto& redirect = session.Requests()[4];
    CHECK(redirect.redirected && redirect.finished && redirect.status == 301 && redirect.responseHeaders[0].value == "https://example.com/new");
    CHECK(session.Requests()[5].url == "https://example.com/new" && session.Requests()[5].type == "XHR" && session.Requests()[5].Pending());
    session.RequestBody(redirect.serial);
    CHECK(redirect.bodyState == BodyState::Failed && Sent(session).empty());
    Respond(session, "page-1", "0.5", "L1", "XHR", 200, "application/json", 12.2);
    CHECK(session.Requests()[5].status == 200 && redirect.status == 301);
    changes = session.TakeChanges();
    CHECK(changes.requests.size() == 2 && changes.requests[0] == redirect.serial);

    // What the memory cache served, and a response to a request never seen.
    session.Receive(Event("page-1", "Network.requestServedFromMemoryCache", {{"requestId", "0.6"}, {"frameId", "f1"}, {"loaderId", "L1"},
        {"documentURL", "https://example.com/"}, {"timestamp", 13}, {"initiator", {{"type", "parser"}}},
        {"resource", {{"url", "https://example.com/style.css"}, {"type", "StyleSheet"}, {"bodySize", 900},
            {"response", {{"url", "https://example.com/style.css"}, {"status", 200}, {"statusText", "OK"}, {"headers", json::object()},
                {"mimeType", "text/css"}, {"source", "memory-cache"}}}}}}));
    CHECK(session.Requests().back().finished && session.Requests().back().source == "memory-cache" && session.Requests().back().size == 900);
    Respond(session, "page-1", "0.99", "L1", "Image", 200, "image/gif", 13.5);
    CHECK(session.Requests().back().url == "ignored" && session.Requests().back().status == 200);
    CHECK(session.Requests().size() == 8);
    CHECK(SummarizeRequests(session.Requests(), 8).find("8 requests") == 0);
    CHECK(SummarizeRequests(session.Requests(), 2).find("2 of 8 requests") == 0);
    CHECK(SummarizeRequests(session.Requests(), 8).find("1 failed") != std::string::npos);
    session.TakeChanges();
    Sent(session);

    // Navigation in the same process: the new document's requests remain.
    Ask(session, "page-1", "0.7", "https://example.com/next", "L2", "Document", 20);
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f1"}, {"loaderId", "L2"}, {"url", "https://example.com/next"},
        {"securityOrigin", "https://example.com"}, {"mimeType", "text/html"}}}}));
    changes = session.TakeChanges();
    CHECK(changes.requestsReset && session.Requests().size() == 1 && session.Requests()[0].url == "https://example.com/next");
    CHECK(session.PageURL() == "https://example.com/next");
    // A frame inside the page is not the page.
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f2"}, {"parentId", "f1"}, {"loaderId", "L3"}, {"url", "https://ads.example/"}}}}));
    CHECK(session.Requests().size() == 1 && session.PageURL() == "https://example.com/next");
    Respond(session, "page-1", "0.7", "L2", "Document", 200, "text/html", 20.1);
    CHECK(session.Requests()[0].status == 200);

    // Kept logs.
    session.SetPreserveRequests(true);
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Network.setClearResourceDataOnNavigate"
        && commands[0].parameters["clearResourceDataOnNavigate"] == false);
    Ask(session, "page-1", "0.8", "https://example.com/third", "L4", "Document", 30);
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f1"}, {"loaderId", "L4"}, {"url", "https://example.com/third"}}}}));
    CHECK(session.Requests().size() == 2 && !session.TakeChanges().requestsReset);

    // A navigation to another process: a provisional target that is paused
    // until its requests are recorded, then takes the page over.
    session.Receive(TargetCreated("page-2", true, true));
    commands = Sent(session);
    CHECK(Find(commands, "Network.enable") && Find(commands, "Network.enable")->target == "page-2");
    CHECK(Find(commands, "Network.setClearResourceDataOnNavigate") && Find(commands, "Target.resume"));
    CHECK(Find(commands, "Target.resume")->parameters["targetId"] == "page-2" && commands.back().method == "Target.resume");
    Ask(session, "page-2", "0.1", "https://other.example/", "L1", "Document", 1);
    session.Receive(json{{"method", "Target.didCommitProvisionalTarget"}, {"params", {{"oldTargetId", "page-1"}, {"newTargetId", "page-2"}}}}.dump());
    session.Receive(json{{"method", "Target.targetDestroyed"}, {"params", {{"targetId", "page-1"}}}}.dump());
    CHECK(session.Requests().size() == 3);
    // The old page's unfinished request will never finish, and its bodies are gone.
    CHECK(session.Requests()[1].failed && session.Requests()[1].canceled);
    session.RequestBody(session.Requests()[0].serial);
    CHECK(session.Requests()[0].bodyState == BodyState::Failed && Sent(session).empty());
    session.SetPreserveRequests(false);
    Sent(session);
    session.TakeChanges();
    session.Receive(TargetCreated("page-3", true, true));
    Ask(session, "page-3", "0.1", "https://third.example/", "L1", "Document", 1);
    Ask(session, "page-2", "0.2", "https://other.example/late.js", "L1", "Script", 2);
    session.Receive(json{{"method", "Target.didCommitProvisionalTarget"}, {"params", {{"oldTargetId", "page-2"}, {"newTargetId", "page-3"}}}}.dump());
    CHECK(session.TakeChanges().requestsReset && session.Requests().size() == 1 && session.Requests()[0].url == "https://third.example/");
    // The same request identifier in another process is another request.
    Respond(session, "page-3", "0.1", "L1", "Document", 200, "text/html", 1.5);
    CHECK(session.Requests()[0].status == 200);

    session.ClearRequests();
    CHECK(session.Requests().empty() && session.TakeChanges().requestsReset);
    CHECK(SummarizeRequests(session.Requests(), 0).find("No requests") == 0);
    Finish(session, "page-3", "0.1", 3, 10);
    CHECK(session.Requests().empty());

    // Nothing that is not the protocol does harm.
    session.Receive("");
    session.Receive("not json");
    session.Receive("[1,2]");
    session.Receive("{\"method\":5,\"params\":\"x\"}");
    session.Receive(Event("page-3", "Network.requestWillBeSent", "text"));
    session.Receive(Event("page-3", "Network.responseReceived", {{"requestId", 5}, {"response", 7}}));
    session.Receive(json{{"method", "Target.dispatchMessageFromTarget"}, {"params", {{"targetId", "page-3"}, {"message", "{broken"}}}}.dump());
    session.Receive(FromTarget("page-3", {{"id", 99999}, {"result", json::object()}}));
    CHECK(session.Requests().empty());

    // More requests than are kept: the oldest go.
    Session many;
    many.Receive(TargetCreated("p"));
    for (size_t i = 0; i < Session::kMaximumRequests + 10; ++i)
        Ask(many, "p", "r" + std::to_string(i), "https://example.com/" + std::to_string(i), "L", "Image", 1);
    CHECK(many.Requests().size() == Session::kMaximumRequests && many.Requests().front().url == "https://example.com/10");
    CHECK(many.TakeChanges().requestsReset);
    CHECK(many.FindRequest(many.Requests()[100].serial) == &many.Requests()[100] && !many.FindRequest(1));
}

static json Message(const std::string& level, const std::string& text, const json& more = json::object())
{
    json message = {{"source", "console-api"}, {"level", level}, {"text", text}, {"type", "log"}, {"timestamp", 1790000000.25}};
    message.update(more);
    return message;
}
static json Value(const json& value)
{
    if (value.is_string()) return {{"type", "string"}, {"value", value}};
    if (value.is_number()) return {{"type", "number"}, {"value", value}, {"description", value.dump()}};
    if (value.is_boolean()) return {{"type", "boolean"}, {"value", value}};
    return {{"type", "object"}, {"subtype", "null"}, {"value", nullptr}};
}

static void TestConsole()
{
    CHECK(ConsoleMessageText(Message("log", "plain").dump()) == "plain");
    CHECK(ConsoleMessageText(Message("log", "a", {{"parameters", {Value("a"), Value(1), Value(true), Value(nullptr), {{"type", "undefined"}}}}}).dump())
        == "a 1 true null undefined");
    CHECK(ConsoleMessageText(Message("log", "x", {{"parameters", {Value("%s is %d years, %i%% %o %c styled %f"), Value("Ann"), Value(41.9),
        Value(7), Value("q"), Value("color: red"), Value(1.5), Value("extra")}}}).dump()) == "Ann is 41 years, 7% \"q\"  styled 1.5 extra");
    CHECK(ConsoleMessageText(Message("log", "x", {{"parameters", {Value("100% done %s %d")}}}).dump()) == "100% done %s %d");
    CHECK(ConsoleMessageText(Message("log", "x", {{"parameters", {Value(5), Value("five")}}}).dump()) == "5 five");
    const json object = {{"type", "object"}, {"className", "Object"}, {"description", "Object"}, {"objectId", "{}"}, {"preview",
        {{"type", "object"}, {"description", "Object"}, {"lossless", false}, {"overflow", true}, {"properties", {
            {{"name", "id"}, {"type", "number"}, {"value", "7"}},
            {{"name", "name"}, {"type", "string"}, {"value", "Ann \"A\""}},
            {{"name", "tags"}, {"type", "object"}, {"subtype", "array"}, {"value", "Array"}, {"valuePreview",
                {{"type", "object"}, {"subtype", "array"}, {"description", "Array"}, {"lossless", true}, {"size", 2}, {"properties", {
                    {{"name", "0"}, {"type", "string"}, {"value", "a"}}, {{"name", "1"}, {"type", "number"}, {"value", "2"}}}}}}},
            {{"name", "none"}, {"type", "object"}, {"subtype", "null"}, {"value", "null"}},
            {{"name", "run"}, {"type", "function"}, {"value", ""}},
            {{"name", "child"}, {"type", "object"}, {"value", "Point"}}}}}}};
    CHECK_TEXT(ConsoleMessageText(Message("log", "x", {{"parameters", {Value("user"), object}}}).dump()),
        "user {id: 7, name: \"Ann \\\"A\\\"\", tags: [\"a\", 2], none: null, run: function, child: Point, …}");
    const json array = {{"type", "object"}, {"subtype", "array"}, {"description", "Array"}, {"size", 100}, {"preview",
        {{"type", "object"}, {"subtype", "array"}, {"description", "Array"}, {"lossless", false}, {"overflow", true}, {"size", 100},
            {"properties", {{{"name", "0"}, {"type", "number"}, {"value", "1"}}, {{"name", "1"}, {"type", "number"}, {"value", "2"}}}}}}};
    CHECK_TEXT(ConsoleMessageText(Message("log", "x", {{"parameters", {array}}}).dump()), "(100) [1, 2, …]");
    const json map = {{"type", "object"}, {"subtype", "map"}, {"description", "Map"}, {"preview",
        {{"type", "object"}, {"subtype", "map"}, {"description", "Map"}, {"lossless", true}, {"entries", {
            {{"key", {{"type", "string"}, {"description", "k"}, {"lossless", true}}}, {"value", {{"type", "number"}, {"description", "1"}, {"lossless", true}}}}}}}}};
    CHECK_TEXT(ConsoleMessageText(Message("log", "x", {{"parameters", {map}}}).dump()), "Map {\"k\" => 1}");
    const json error = {{"type", "object"}, {"subtype", "error"}, {"className", "TypeError"}, {"description", "TypeError: x is not a function"},
        {"preview", {{"type", "object"}, {"subtype", "error"}, {"description", "TypeError: x is not a function"}, {"lossless", false}}}};
    CHECK_TEXT(ConsoleMessageText(Message("error", "x", {{"parameters", {error}}}).dump()), "TypeError: x is not a function");
    CHECK(ConsoleMessageText(Message("log", "x", {{"parameters", {{{"type", "function"}, {"description", "function f() {\n  return 1;\n}"}}}}}).dump())
        == "function f() {…");
    CHECK(ConsoleMessageText(Message("error", "was false", {{"type", "assert"}}).dump()) == "Assertion failed: was false");
    CHECK(ConsoleMessageText("{broken").empty());

    Session session;
    session.Start();
    session.Receive(TargetCreated("page-1"));
    Sent(session);
    const json stack = {{"callFrames", {
        {{"functionName", "inner"}, {"url", "https://example.com/js/app.js"}, {"scriptId", "1"}, {"lineNumber", 12}, {"columnNumber", 5}},
        {{"functionName", ""}, {"url", "https://example.com/js/app.js"}, {"scriptId", "1"}, {"lineNumber", 30}, {"columnNumber", 1}},
        {{"functionName", "forEach"}, {"url", ""}, {"scriptId", "0"}, {"lineNumber", 0}, {"columnNumber", 0}}}}};
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("debug", "fine detail")}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "hello", {{"url", "https://example.com/js/app.js"}, {"line", 3}, {"column", 9}})}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("info", "note")}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("warning", "careful", {{"stackTrace", stack}})}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("error", "TypeError: no", {{"source", "javascript"}, {"stackTrace", stack}})}}));
    auto changes = session.TakeChanges();
    CHECK(changes.consoleAdded == 5 && !changes.consoleReset && !changes.consoleLastChanged);
    const auto& entries = session.Entries();
    CHECK(entries.size() == 5 && entries[0].level == Level::Debug && entries[1].level == Level::Log && entries[2].level == Level::Info
        && entries[3].level == Level::Warning && entries[4].level == Level::Error);
    CHECK(entries[1].Location() == "app.js:3:9" && entries[1].stack.empty() && entries[1].time == 1790000000.25);
    CHECK_TEXT(entries[4].stack, "inner (app.js:12:5)\n(anonymous) (app.js:30:1)\nforEach (native code)");
    // Without a place of its own, a message is where its first frame is.
    CHECK(entries[3].Location() == "app.js:12:5" && entries[4].source == "javascript" && entries[0].Location().empty());

    LevelFilter filter;
    CHECK(filter.Shows(entries[0]) && filter.Shows(entries[4]));
    filter.info = false;
    CHECK(!filter.Shows(entries[1]) && !filter.Shows(entries[2]) && filter.Shows(entries[0]) && filter.Shows(entries[3]));
    filter = { };
    filter.debug = filter.errors = filter.warnings = false;
    CHECK(!filter.Shows(entries[0]) && filter.Shows(entries[1]) && !filter.Shows(entries[3]) && !filter.Shows(entries[4]));

    // The same message again is counted, not repeated.
    session.Receive(Event("page-1", "Console.messageRepeatCountUpdated", {{"count", 3}, {"timestamp", 1790000001}}));
    changes = session.TakeChanges();
    CHECK(entries.back().repeat == 3 && changes.consoleLastChanged && !changes.consoleAdded && !changes.consoleReset);
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "again")}}));
    session.Receive(Event("page-1", "Console.messageRepeatCountUpdated", {{"count", 2}}));
    changes = session.TakeChanges();
    CHECK(entries.back().repeat == 2 && changes.consoleAdded == 1 && !changes.consoleLastChanged && !changes.consoleReset);

    // Groups stand in.
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "Group", {{"type", "startGroup"}})}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "two\nlines")}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "", {{"type", "endGroup"}})}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "after")}}));
    CHECK(entries.size() == 9 && entries[6].text == "Group" && entries[7].text == "  two\n  lines" && entries[8].text == "after");

    // An expression typed into the console.
    session.TakeChanges();
    session.Evaluate("1 + 1");
    auto commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Runtime.evaluate" && commands[0].parameters["expression"] == "1 + 1"
        && commands[0].target == "page-1");
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"result", {{"result", Value(2)}, {"wasThrown", false}}}}));
    CHECK(entries.size() == 11 && entries[9].kind == EntryKind::Input && entries[9].text == "1 + 1"
        && entries[10].kind == EntryKind::Result && entries[10].text == "2" && entries[10].level == Level::Log);
    session.Evaluate("'a' +");
    commands = Sent(session);
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"result", {{"result", error}, {"wasThrown", true}}}}));
    CHECK(entries.back().level == Level::Error && entries.back().text == "TypeError: x is not a function");
    session.Evaluate("'text'");
    commands = Sent(session);
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"result", {{"result", Value("text")}, {"wasThrown", false}}}}));
    CHECK(entries.back().text == "\"text\"");
    // Results are shown whatever levels are.
    filter = { };
    filter.info = filter.errors = false;
    CHECK(filter.Shows(entries.back()) && filter.Shows(entries[9]));

    // Navigation clears the console, unless it is kept.
    session.TakeChanges();
    session.Receive(Event("page-1", "Console.messagesCleared", {{"reason", "main-frame-navigation"}}));
    changes = session.TakeChanges();
    CHECK(entries.empty() && changes.consoleReset);
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f1"}, {"loaderId", "L2"}, {"url", "https://example.com/two"}}}}));
    CHECK(entries.empty());
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "kept")}}));
    session.SetPreserveConsole(true);
    session.Receive(Event("page-1", "Console.messagesCleared", {{"reason", "main-frame-navigation"}}));
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f1"}, {"loaderId", "L3"}, {"url", "https://example.com/three"}}}}));
    CHECK(entries.size() == 2 && entries[0].text == "kept" && entries[1].kind == EntryKind::Navigation
        && entries[1].text == "Navigated to https://example.com/three");
    session.Receive(Event("page-1", "Console.messagesCleared", {{"reason", "console-api"}}));
    CHECK(entries.size() == 3 && entries[2].level == Level::Info);
    session.Receive(TargetCreated("page-2", true, true));
    session.Receive(json{{"method", "Target.didCommitProvisionalTarget"}, {"params", {{"oldTargetId", "page-1"}, {"newTargetId", "page-2"}}}}.dump());
    CHECK(entries.size() == 3);
    Sent(session);
    session.Evaluate("2");
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].target == "page-2");
    session.SetPreserveConsole(false);
    session.Receive(Event("page-2", "Console.messagesCleared", {{"reason", "console-api"}}));
    CHECK(entries.empty());
    session.Receive(Event("page-2", "Console.messageAdded", {{"message", Message("log", "x")}}));
    session.Receive(TargetCreated("page-3", true, false));
    session.Receive(json{{"method", "Target.didCommitProvisionalTarget"}, {"params", {{"oldTargetId", "page-2"}, {"newTargetId", "page-3"}}}}.dump());
    CHECK(entries.empty());
    session.Receive(Event("page-3", "Console.messageAdded", {{"message", Message("log", "y")}}));
    session.TakeChanges();
    session.ClearConsole();
    CHECK(entries.empty() && session.TakeChanges().consoleReset);
    session.Receive(Event("page-3", "Console.messagesCleared", {{"reason", "frontend"}}));
    CHECK(!session.TakeChanges().Any());

    // Without a page, an expression is answered at once.
    Session empty;
    empty.Evaluate("1");
    CHECK(empty.Entries().size() == 2 && empty.Entries()[1].level == Level::Error && empty.TakeOutgoing().empty());

    Session many;
    many.Receive(TargetCreated("p"));
    for (size_t i = 0; i < Session::kMaximumEntries + 5; ++i)
        many.Receive(Event("p", "Console.messageAdded", {{"message", Message("log", std::to_string(i))}}));
    CHECK(many.Entries().size() == Session::kMaximumEntries && many.Entries().front().text == "5" && many.TakeChanges().consoleReset);
}

static void TestHeaders()
{
    Session session;
    session.Receive(TargetCreated("p"));
    Ask(session, "p", "1", "https://example.com/api?x=1", "L", "XHR", 5);
    Respond(session, "p", "1", "L", "XHR", 200, "application/json", 5.25);
    Finish(session, "p", "1", 5.5, 2048);
    const auto described = DescribeHeaders(session.Requests()[0]);
    const auto& text = described.text;
    CHECK(text.find("General\n  URL: https://example.com/api?x=1\n  Method: GET\n  Status: 200 OK\n  Type: XHR (application/json)\n") == 0);
    CHECK(text.find("  Remote address: 93.184.216.34:443\n  Protocol: h2\n") != std::string::npos);
    CHECK(text.find("  Size: 2.0 KB (1.1 KB transferred)\n") != std::string::npos);
    CHECK(text.find("  Time: 500 ms (250 ms to the first byte)\n") != std::string::npos);
    CHECK(text.find("\n\nResponse Headers (2)\n  Cache-Control: no-store\n  content-type: application/json\n\nRequest Headers (3)\n  Accept: */*\n") != std::string::npos);
    CHECK(text.back() != '\n');
    bool title = false, good = false;
    for (const auto& run : described.runs) {
        CHECK(run.offset + run.length <= text.size());
        const auto part = text.substr(run.offset, run.length);
        if (run.style == TextStyle::Title && part == "Response Headers (2)") title = true;
        if (run.style == TextStyle::Good && part == "200 OK") good = true;
    }
    CHECK(title && good);
    session.Receive(Event("p", "Network.requestWillBeSent", {{"requestId", "2"}, {"loaderId", "L"}, {"request", {{"url", "https://example.com/x"}, {"method", "GET"}, {"headers", json::object()}}}, {"timestamp", 6}}));
    CHECK(DescribeHeaders(session.Requests()[1]).text.find("  Status: Pending\n") != std::string::npos);
    session.Receive(Event("p", "Network.loadingFailed", {{"requestId", "2"}, {"timestamp", 7}, {"errorText", "Could not resolve host"}}));
    CHECK(DescribeHeaders(session.Requests()[1]).text.find("  Status: Failed: Could not resolve host\n") != std::string::npos);
    CHECK(session.Requests()[1].IsError() && session.Requests()[1].StatusLabel() == "(failed)" && session.Requests()[1].type == "Other");
    summit::devtools::Request data;
    data.url = "data:image/png;base64," + std::string(200, 'A');
    CHECK(data.Name().size() < 60 && data.Host().empty());
    data.url = "https://user:pw@example.com:8443/a/b/?q#f";
    CHECK(data.Host() == "example.com:8443" && data.Name() == "b?q");
    data.url = "about:blank";
    CHECK(data.Name() == "about:blank");
}

int main()
{
    TestSession();
    TestConsole();
    TestHeaders();
    TestKinds();
    TestJSON();
    TestXML();
    TestHTML();
    TestHelpers();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
