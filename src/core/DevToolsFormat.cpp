#include "DevToolsFormat.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <vector>

namespace summit::devtools {
namespace {
// Bodies over this are not parsed to find out what they are.
constexpr size_t kSniffLimit = 16 * 1024 * 1024;
// An element is written on one line when that line is no longer than this.
constexpr size_t kCompactLine = 120;
// Deeper elements are not indented further; the text would leave the view.
constexpr int kDeepestIndent = 40;

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
bool IsNameStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || static_cast<unsigned char>(c) >= 0x80; }

std::string LowerCopy(std::string_view text)
{
    std::string result(text);
    for (auto& c : result) c = Lower(c);
    return result;
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
    return text;
}

bool StartsWith(std::string_view text, std::string_view prefix) { return text.substr(0, prefix.size()) == prefix; }
bool EndsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool StartsWithNoCase(std::string_view text, std::string_view lowerPrefix)
{
    if (text.size() < lowerPrefix.size()) return false;
    for (size_t i = 0; i < lowerPrefix.size(); ++i)
        if (Lower(text[i]) != lowerPrefix[i]) return false;
    return true;
}

size_t FindNoCase(std::string_view text, std::string_view lowerNeedle, size_t from)
{
    if (lowerNeedle.empty() || text.size() < lowerNeedle.size()) return std::string_view::npos;
    for (size_t i = from; i + lowerNeedle.size() <= text.size(); ++i)
        if (Lower(text[i]) == lowerNeedle[0] && StartsWithNoCase(text.substr(i), lowerNeedle)) return i;
    return std::string_view::npos;
}

bool Contains(std::initializer_list<std::string_view> names, std::string_view name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::string_view SkipBOM(std::string_view text)
{
    return StartsWith(text, "\xEF\xBB\xBF") ? text.substr(3) : text;
}

BodyKind Sniff(std::string_view body)
{
    const auto text = Trim(SkipBOM(body));
    if (text.empty()) return BodyKind::Text;
    if ((text.front() == '{' || text.front() == '[') && text.size() <= kSniffLimit && nlohmann::json::accept(text))
        return BodyKind::JSON;
    if (StartsWithNoCase(text, "<!doctype html") || StartsWithNoCase(text, "<html")) return BodyKind::HTML;
    if (StartsWith(text, "<?xml") || StartsWithNoCase(text, "<svg") || StartsWithNoCase(text, "<rss")
        || StartsWithNoCase(text, "<feed")) return BodyKind::XML;
    const auto head = body.substr(0, 1024);
    if (head.find('\0') != std::string_view::npos) return BodyKind::Binary;
    return BodyKind::Text;
}

// Lines of the output. Every line ends with a line feed until Finish().
class Lines {
public:
    explicit Lines(size_t reserve) { fText.reserve(reserve + reserve / 4); }
    void Add(int depth, std::string_view text)
    {
        fText.append(static_cast<size_t>(std::clamp(depth, 0, kDeepestIndent)) * 2, ' ');
        fText.append(text);
        fText += '\n';
    }
    // Text of several lines: each is trimmed and takes the indentation.
    void AddText(int depth, std::string_view text)
    {
        while (!text.empty()) {
            const auto end = text.find('\n');
            const auto line = Trim(text.substr(0, end));
            if (!line.empty()) Add(depth, line);
            if (end == std::string_view::npos) break;
            text.remove_prefix(end + 1);
        }
    }
    void Blank() { fText += '\n'; }
    std::string Finish()
    {
        while (!fText.empty() && fText.back() == '\n') fText.pop_back();
        return std::move(fText);
    }
private:
    std::string fText;
};

enum class TokenType { Text, StartTag, EndTag, EmptyTag, Comment, CData, Instruction, Declaration, RawText, Verbatim };
struct Token {
    TokenType type;
    std::string_view text;
    // Of tags; lower case in HTML.
    std::string name;
};

// The end of the tag that starts at position: after its '>', which a quoted
// attribute value may contain.
size_t TagEnd(std::string_view text, size_t position)
{
    char quote = 0;
    for (size_t i = position + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') quote = c;
        else if (c == '>') return i + 1;
    }
    return std::string_view::npos;
}

// <!DOCTYPE ...>, whose internal subset may hold '>' between brackets.
size_t DeclarationEnd(std::string_view text, size_t position)
{
    char quote = 0;
    int brackets = 0;
    for (size_t i = position + 2; i < text.size(); ++i) {
        const char c = text[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') quote = c;
        else if (c == '[') ++brackets;
        else if (c == ']') brackets = std::max(0, brackets - 1);
        else if (c == '>' && !brackets) return i + 1;
    }
    return std::string_view::npos;
}

std::string TagName(std::string_view tag, size_t from, bool html)
{
    size_t end = from;
    while (end < tag.size() && !IsSpace(tag[end]) && tag[end] != '>' && tag[end] != '/') ++end;
    return html ? LowerCopy(tag.substr(from, end - from)) : std::string(tag.substr(from, end - from));
}

std::vector<Token> Tokenize(std::string_view text, bool html)
{
    std::vector<Token> tokens;
    size_t textStart = 0, i = 0;
    const auto flushText = [&](size_t end) {
        if (end > textStart) tokens.push_back({TokenType::Text, text.substr(textStart, end - textStart), { }});
    };
    const auto add = [&](TokenType type, size_t end, std::string name = { }) {
        flushText(i);
        tokens.push_back({type, text.substr(i, end - i), std::move(name)});
        i = textStart = end;
    };
    while (i < text.size()) {
        if (text[i] != '<' || i + 1 >= text.size()) { ++i; continue; }
        const auto rest = text.substr(i);
        const char next = text[i + 1];
        if (StartsWith(rest, "<!--")) {
            const auto end = text.find("-->", i + 4);
            add(TokenType::Comment, end == std::string_view::npos ? text.size() : end + 3);
        } else if (StartsWith(rest, "<![CDATA[")) {
            const auto end = text.find("]]>", i + 9);
            add(TokenType::CData, end == std::string_view::npos ? text.size() : end + 3);
        } else if (next == '?') {
            const auto end = html ? text.find('>', i) : text.find("?>", i + 2);
            add(TokenType::Instruction, end == std::string_view::npos ? text.size() : end + (html ? 1 : 2));
        } else if (next == '!') {
            const auto end = DeclarationEnd(text, i);
            add(TokenType::Declaration, end == std::string_view::npos ? text.size() : end);
        } else if (next == '/' && i + 2 < text.size() && IsNameStart(text[i + 2])) {
            const auto end = text.find('>', i);
            if (end == std::string_view::npos) { ++i; continue; }
            add(TokenType::EndTag, end + 1, TagName(text.substr(i, end + 1 - i), 2, html));
        } else if (IsNameStart(next)) {
            const auto end = TagEnd(text, i);
            if (end == std::string_view::npos) { ++i; continue; }
            const auto tag = text.substr(i, end - i);
            auto name = TagName(tag, 1, html);
            const bool empty = tag.size() >= 3 && tag[tag.size() - 2] == '/';
            if (html && !empty && (name == "pre" || name == "textarea")) {
                // White space counts in these: from start tag to end tag as it is.
                const auto close = FindNoCase(text, "</" + name, end);
                const auto closeEnd = close == std::string_view::npos ? close : text.find('>', close);
                add(TokenType::Verbatim, closeEnd == std::string_view::npos ? text.size() : closeEnd + 1, name);
            } else if (html && !empty && (name == "script" || name == "style")) {
                add(TokenType::StartTag, end, name);
                const auto close = FindNoCase(text, "</" + name, end);
                i = close == std::string_view::npos ? text.size() : close;
                if (i > end) tokens.push_back({TokenType::RawText, text.substr(end, i - end), name});
                textStart = i;
            } else
                add(empty ? TokenType::EmptyTag : TokenType::StartTag, end, std::move(name));
        } else
            ++i;
    }
    i = text.size();
    flushText(i);
    return tokens;
}

bool IsBlank(std::string_view text)
{
    return std::all_of(text.begin(), text.end(), IsSpace);
}

// A tag as one line: line breaks between attributes become single spaces.
std::string OneLineTag(std::string_view tag)
{
    if (tag.find('\n') == std::string_view::npos && tag.find('\r') == std::string_view::npos) return std::string(tag);
    std::string result;
    result.reserve(tag.size());
    char quote = 0;
    bool space = false;
    for (const char c : tag) {
        if (quote) {
            result += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (IsSpace(c)) { space = true; continue; }
        if (space && !result.empty() && c != '>') result += ' ';
        space = false;
        if (c == '"' || c == '\'') quote = c;
        result += c;
    }
    return result;
}

// Script and style contents: the indentation their lines share is replaced
// by the element's, and blank lines at either end are dropped.
void AddRawText(Lines& lines, int depth, std::string_view text)
{
    std::vector<std::string_view> rows;
    while (true) {
        const auto end = text.find('\n');
        auto row = text.substr(0, end);
        while (!row.empty() && (row.back() == '\r' || row.back() == ' ' || row.back() == '\t')) row.remove_suffix(1);
        rows.push_back(row);
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    while (!rows.empty() && rows.back().empty()) rows.pop_back();
    size_t first = 0;
    while (first < rows.size() && rows[first].empty()) ++first;
    size_t shared = std::string_view::npos;
    for (size_t i = first; i < rows.size(); ++i) {
        if (rows[i].empty()) continue;
        size_t indent = 0;
        while (indent < rows[i].size() && (rows[i][indent] == ' ' || rows[i][indent] == '\t')) ++indent;
        shared = std::min(shared, indent);
    }
    for (size_t i = first; i < rows.size(); ++i) {
        if (rows[i].empty()) lines.Blank();
        else lines.Add(depth, rows[i].substr(shared));
    }
}

bool IsVoid(std::string_view name)
{
    return Contains({"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param",
        "source", "track", "wbr"}, name);
}

// Elements that stay in the line of text around them.
bool IsInline(std::string_view name)
{
    return Contains({"a", "abbr", "b", "bdi", "bdo", "big", "br", "cite", "code", "data", "del", "dfn", "em", "font",
        "i", "img", "ins", "kbd", "mark", "nobr", "q", "s", "samp", "small", "span", "strike", "strong", "sub",
        "sup", "time", "tt", "u", "var", "wbr"}, name);
}

// Elements whose start tag ends an open paragraph.
bool ClosesParagraph(std::string_view name)
{
    return Contains({"address", "article", "aside", "blockquote", "details", "div", "dl", "fieldset", "figcaption",
        "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup", "hr", "main", "menu",
        "nav", "ol", "p", "pre", "section", "table", "ul"}, name);
}

// Builds a line of text and inline tags, with runs of white space as one space.
class Flow {
public:
    void AddText(std::string_view text)
    {
        for (const char c : text) {
            if (IsSpace(c)) { fSpace = true; continue; }
            Separate();
            fLine += c;
        }
    }
    void AddTag(std::string_view tag)
    {
        Separate();
        fLine += OneLineTag(tag);
    }
    bool Empty() const { return fLine.empty(); }
    size_t Size() const { return fLine.size(); }
    std::string Take()
    {
        fSpace = false;
        return std::exchange(fLine, { });
    }
private:
    void Separate()
    {
        if (fSpace && !fLine.empty()) fLine += ' ';
        fSpace = false;
    }
    std::string fLine;
    bool fSpace = false;
};

class HTMLFormatter {
public:
    explicit HTMLFormatter(std::string_view text)
        : fTokens(Tokenize(text, true)), fLines(text.size())
    {
    }
    std::string Run()
    {
        for (size_t i = 0; i < fTokens.size(); ++i) {
            const auto& token = fTokens[i];
            switch (token.type) {
                case TokenType::Text: fFlow.AddText(token.text); break;
                case TokenType::StartTag: case TokenType::EmptyTag: i = Start(i); break;
                case TokenType::EndTag: End(token); break;
                case TokenType::RawText: AddRawText(fLines, Depth(), token.text); break;
                case TokenType::Verbatim: Flush(); fLines.Add(Depth(), token.text); break;
                default: Flush(); fLines.Add(Depth(), token.text); break;
            }
        }
        Flush();
        return fLines.Finish();
    }
private:
    int Depth() const { return static_cast<int>(fOpen.size()); }
    void Flush()
    {
        if (fFlow.Empty()) { fFlow.Take(); return; }
        fLines.Add(Depth(), fFlow.Take());
    }
    // Ends the nearest open element among targets, with everything open
    // inside it, unless one of boundaries is nearer.
    void CloseUntil(std::initializer_list<std::string_view> targets, std::initializer_list<std::string_view> boundaries)
    {
        for (size_t i = fOpen.size(); i-- > 0;) {
            if (Contains(boundaries, fOpen[i])) return;
            if (Contains(targets, fOpen[i])) { fOpen.resize(i); return; }
        }
    }
    // The end tags HTML lets authors leave out.
    void ImplyEnds(std::string_view name)
    {
        if (name == "li") CloseUntil({"li"}, {"ul", "ol", "menu"});
        else if (name == "dt" || name == "dd") CloseUntil({"dt", "dd"}, {"dl"});
        else if (name == "tr") CloseUntil({"tr"}, {"table", "thead", "tbody", "tfoot"});
        else if (name == "td" || name == "th") CloseUntil({"td", "th"}, {"tr", "table"});
        else if (name == "thead" || name == "tbody" || name == "tfoot") CloseUntil({"thead", "tbody", "tfoot"}, {"table"});
        else if (name == "option") CloseUntil({"option"}, {"select", "optgroup", "datalist"});
        else if (name == "optgroup") CloseUntil({"optgroup"}, {"select"});
        else if (name == "body") CloseUntil({"head"}, {"html"});
        if (ClosesParagraph(name) && !fOpen.empty() && fOpen.back() == "p") fOpen.pop_back();
    }
    // The index of the end tag of the element that starts at index, if all
    // between is text and inline elements and fits one line; else 0.
    size_t CompactEnd(size_t index, std::string& line)
    {
        Flow flow;
        flow.AddTag(fTokens[index].text);
        for (size_t i = index + 1; i < fTokens.size(); ++i) {
            const auto& token = fTokens[i];
            if (flow.Size() > kCompactLine) return 0;
            if (token.type == TokenType::Text) {
                flow.AddText(token.text);
                continue;
            }
            const bool tag = token.type == TokenType::StartTag || token.type == TokenType::EndTag
                || token.type == TokenType::EmptyTag;
            if (!tag) return 0;
            if (token.type == TokenType::EndTag && token.name == fTokens[index].name) {
                // No space between the text and the tags around it.
                line = flow.Take();
                line += OneLineTag(token.text);
                return line.size() <= kCompactLine ? i : 0;
            }
            if (!IsInline(token.name) || token.name == "br") return 0;
            flow.AddTag(token.text);
        }
        return 0;
    }
    size_t Start(size_t index)
    {
        const auto& token = fTokens[index];
        if (IsInline(token.name)) {
            fFlow.AddTag(token.text);
            if (token.name == "br") Flush();
            return index;
        }
        Flush();
        ImplyEnds(token.name);
        if (token.type == TokenType::EmptyTag || IsVoid(token.name)) {
            fLines.Add(Depth(), OneLineTag(token.text));
            return index;
        }
        std::string line;
        if (token.name != "script" && token.name != "style") {
            if (const auto end = CompactEnd(index, line)) {
                fLines.Add(Depth(), line);
                return end;
            }
        }
        fLines.Add(Depth(), OneLineTag(token.text));
        fOpen.push_back(token.name);
        return index;
    }
    void End(const Token& token)
    {
        if (IsInline(token.name)) {
            fFlow.AddTag(token.text);
            return;
        }
        Flush();
        const auto found = std::find(fOpen.rbegin(), fOpen.rend(), token.name);
        if (found != fOpen.rend()) fOpen.erase(std::prev(found.base()), fOpen.end());
        fLines.Add(Depth(), OneLineTag(token.text));
    }
    std::vector<Token> fTokens;
    Lines fLines;
    Flow fFlow;
    std::vector<std::string> fOpen;
};

// The length of a prefix that servers put before JSON so that it cannot be
// run as a script, with the white space that follows it; 0 if there is none.
size_t GuardPrefix(std::string_view text)
{
    for (const std::string_view prefix : {")]}',", ")]}'", "while(1);", "while (1);", "for(;;);", "for (;;);", "{}&&"}) {
        if (!StartsWith(text, prefix)) continue;
        size_t length = prefix.size();
        while (length < text.size() && IsSpace(text[length])) ++length;
        return length;
    }
    return 0;
}
}

BodyKind DetectBodyKind(std::string_view mimeType, std::string_view body)
{
    const auto mime = LowerCopy(Trim(mimeType.substr(0, mimeType.find(';'))));
    if (mime == "application/json" || mime == "text/json" || mime == "application/x-json" || EndsWith(mime, "+json"))
        return BodyKind::JSON;
    if (mime == "text/html" || mime == "application/xhtml+xml") return BodyKind::HTML;
    if (mime == "text/xml" || mime == "application/xml" || EndsWith(mime, "+xml")) return BodyKind::XML;
    if (mime == "text/javascript" || mime == "application/javascript" || mime == "application/x-javascript"
        || mime == "application/ecmascript" || mime == "text/ecmascript" || mime == "text/jscript")
        return BodyKind::JavaScript;
    if (mime == "text/css") return BodyKind::CSS;
    if (StartsWith(mime, "image/")) return BodyKind::Image;
    if (StartsWith(mime, "audio/") || StartsWith(mime, "video/") || StartsWith(mime, "font/")
        || mime == "application/wasm" || mime == "application/zip" || mime == "application/pdf"
        || mime == "application/gzip" || mime == "application/font-woff" || mime == "application/font-woff2")
        return BodyKind::Binary;
    const auto sniffed = Sniff(body);
    // A type that says text is believed about everything but what text it is.
    if (StartsWith(mime, "text/") && sniffed == BodyKind::Binary) return BodyKind::Text;
    return sniffed;
}

const char* BodyKindName(BodyKind kind)
{
    switch (kind) {
        case BodyKind::JSON: return "JSON";
        case BodyKind::XML: return "XML";
        case BodyKind::HTML: return "HTML";
        case BodyKind::JavaScript: return "JavaScript";
        case BodyKind::CSS: return "CSS";
        case BodyKind::Image: return "Image";
        case BodyKind::Binary: return "Binary";
        default: return "Text";
    }
}

const char* BodyKindLexer(BodyKind kind)
{
    switch (kind) {
        case BodyKind::JSON: return "json";
        case BodyKind::XML: return "xml";
        case BodyKind::HTML: return "hypertext";
        case BodyKind::JavaScript: return "cpp";
        case BodyKind::CSS: return "css";
        default: return "null";
    }
}

bool CanFormat(BodyKind kind)
{
    return kind == BodyKind::JSON || kind == BodyKind::XML || kind == BodyKind::HTML;
}

FormatResult FormatBody(BodyKind kind, std::string_view body)
{
    FormatResult result;
    switch (kind) {
        case BodyKind::JSON:
            result.formatted = FormatJSON(body, result.text);
            if (!result.formatted) result.note = "Not valid JSON: shown as received";
            break;
        case BodyKind::XML:
            result.text = FormatXML(body);
            result.formatted = true;
            break;
        case BodyKind::HTML:
            result.text = FormatHTML(body);
            result.formatted = true;
            break;
        default: break;
    }
    if (!result.formatted) result.text = std::string(body);
    return result;
}

bool FormatJSON(std::string_view input, std::string& out)
{
    auto text = Trim(SkipBOM(input));
    const auto guard = GuardPrefix(text);
    const auto prefix = Trim(text.substr(0, guard));
    text.remove_prefix(guard);
    if (text.empty() || !nlohmann::json::accept(text)) return false;
    std::string result;
    result.reserve(text.size() + text.size() / 2);
    if (!prefix.empty()) {
        result += prefix;
        result += '\n';
    }
    int depth = 0;
    const auto newLine = [&] {
        result += '\n';
        result.append(static_cast<size_t>(std::min(depth, kDeepestIndent)) * 2, ' ');
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            // The text is valid, so the string ends.
            size_t end = i + 1;
            while (text[end] != '"') end += text[end] == '\\' ? 2 : 1;
            result.append(text.substr(i, end + 1 - i));
            i = end;
        } else if (c == '{' || c == '[') {
            size_t next = i + 1;
            while (IsSpace(text[next])) ++next;
            result += c;
            if (text[next] == '}' || text[next] == ']') {
                result += text[next];
                i = next;
            } else {
                ++depth;
                newLine();
            }
        } else if (c == '}' || c == ']') {
            --depth;
            newLine();
            result += c;
        } else if (c == ',') {
            result += c;
            newLine();
        } else if (c == ':') result += ": ";
        else if (!IsSpace(c)) result += c;
    }
    out = std::move(result);
    return true;
}

std::string FormatXML(std::string_view input)
{
    const auto text = SkipBOM(input);
    const auto tokens = Tokenize(text, false);
    Lines lines(text.size());
    int depth = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        switch (token.type) {
            case TokenType::Text:
                if (!IsBlank(token.text)) lines.AddText(depth, token.text);
                break;
            case TokenType::StartTag: {
                const auto closes = [&](size_t index) {
                    return index < tokens.size() && tokens[index].type == TokenType::EndTag && tokens[index].name == token.name;
                };
                auto line = OneLineTag(token.text);
                if (closes(i + 1)) {
                    lines.Add(depth, line + OneLineTag(tokens[i + 1].text));
                    i += 1;
                    break;
                }
                if (i + 1 < tokens.size() && closes(i + 2)
                    && (tokens[i + 1].type == TokenType::Text || tokens[i + 1].type == TokenType::CData)) {
                    const auto content = tokens[i + 1].type == TokenType::Text ? Trim(tokens[i + 1].text) : tokens[i + 1].text;
                    if (content.find('\n') == std::string_view::npos) {
                        line += content;
                        lines.Add(depth, line + OneLineTag(tokens[i + 2].text));
                        i += 2;
                        break;
                    }
                }
                lines.Add(depth++, line);
                break;
            }
            case TokenType::EndTag:
                depth = std::max(0, depth - 1);
                lines.Add(depth, OneLineTag(token.text));
                break;
            case TokenType::EmptyTag: lines.Add(depth, OneLineTag(token.text)); break;
            default: lines.Add(depth, token.text); break;
        }
    }
    return lines.Finish();
}

std::string FormatHTML(std::string_view text)
{
    return HTMLFormatter(SkipBOM(text)).Run();
}

std::string FormatBytes(uint64_t bytes)
{
    char buffer[48];
    if (bytes < 1024) std::snprintf(buffer, sizeof buffer, "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024 * 1024) std::snprintf(buffer, sizeof buffer, "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) std::snprintf(buffer, sizeof buffer, "%.2f MB", bytes / (1024.0 * 1024));
    else std::snprintf(buffer, sizeof buffer, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    return buffer;
}

std::string FormatDuration(double seconds)
{
    if (!(seconds >= 0)) return { };
    char buffer[48];
    if (seconds < 0.9995) std::snprintf(buffer, sizeof buffer, "%.0f ms", seconds * 1000);
    else if (seconds < 60) std::snprintf(buffer, sizeof buffer, "%.2f s", seconds);
    else std::snprintf(buffer, sizeof buffer, "%.1f min", seconds / 60);
    return buffer;
}

bool DecodeBase64(std::string_view text, std::string& out)
{
    std::string result;
    result.reserve(text.size() / 4 * 3 + 3);
    uint32_t bits = 0;
    int count = 0, padding = 0;
    for (const char c : text) {
        if (IsSpace(c)) continue;
        int value;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+' || c == '-') value = 62;
        else if (c == '/' || c == '_') value = 63;
        else if (c == '=') { ++padding; continue; }
        else return false;
        if (padding) return false;
        bits = bits << 6 | static_cast<uint32_t>(value);
        if (++count == 4) {
            result += static_cast<char>(bits >> 16);
            result += static_cast<char>(bits >> 8);
            result += static_cast<char>(bits);
            bits = 0;
            count = 0;
        }
    }
    if (count == 1 || padding > 2) return false;
    if (count == 2) result += static_cast<char>(bits >> 4);
    else if (count == 3) {
        result += static_cast<char>(bits >> 10);
        result += static_cast<char>(bits >> 2);
    }
    out = std::move(result);
    return true;
}

std::string HexDump(std::string_view bytes, size_t limit)
{
    const auto shown = bytes.substr(0, limit);
    std::string result;
    result.reserve(shown.size() / 16 * 78 + 96);
    char buffer[32];
    for (size_t row = 0; row < shown.size(); row += 16) {
        std::snprintf(buffer, sizeof buffer, "%08zx  ", row);
        result += buffer;
        for (size_t i = 0; i < 16; ++i) {
            if (row + i < shown.size()) {
                std::snprintf(buffer, sizeof buffer, "%02x ", static_cast<unsigned char>(shown[row + i]));
                result += buffer;
            } else result += "   ";
            if (i == 7) result += ' ';
        }
        result += ' ';
        for (size_t i = 0; i < 16 && row + i < shown.size(); ++i) {
            const auto c = static_cast<unsigned char>(shown[row + i]);
            result += c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '.';
        }
        result += '\n';
    }
    if (shown.size() < bytes.size())
        result += "… " + FormatBytes(bytes.size() - shown.size()) + " more\n";
    if (!result.empty()) result.pop_back();
    return result;
}

std::string ValidUTF8(std::string_view text)
{
    const auto bad = [](unsigned char c) { return (c & 0xC0) != 0x80; };
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        size_t length = 0;
        if (c && c < 0x80) length = 1;
        else if (c >= 0xC2 && c <= 0xDF) length = 2;
        else if (c >= 0xE0 && c <= 0xEF) length = 3;
        else if (c >= 0xF0 && c <= 0xF4) length = 4;
        bool valid = length && i + length <= text.size();
        for (size_t k = 1; valid && k < length; ++k) valid = !bad(static_cast<unsigned char>(text[i + k]));
        if (valid && length > 2) {
            const auto second = static_cast<unsigned char>(text[i + 1]);
            // Overlong forms, surrogates and what lies beyond U+10FFFF.
            if ((c == 0xE0 && second < 0xA0) || (c == 0xED && second >= 0xA0)
                || (c == 0xF0 && second < 0x90) || (c == 0xF4 && second >= 0x90)) valid = false;
        }
        if (!valid) {
            result += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        result.append(text.substr(i, length));
        i += length;
    }
    return result;
}
}
