#include "core.hpp"
#include "expat.h"
#include <type_traits>

namespace cab {
const Scalar *Node::findProperty(std::string_view key) const {
    const Scalar *out = nullptr;
    auto accept = [&](const Scalar &property) {
        if (out) throw Error("Duplicate property " + std::string(key));
        out = &property;
    };
    for (auto &p : props)
        if (p.name == key) accept(p);
    for (auto &c : children)
        if (c.name == key && c.text && c.props.empty() && c.children.empty()) accept(*c.text);
    return out;
}
const Scalar &Node::property(std::string_view key) const {
    auto *found = findProperty(key);
    if (!found) throw Error("Expected exactly one property " + std::string(key));
    return *found;
}
// Scalar offsets include their encoded length so only that value needs replacement.
struct BinaryReader {
    const Bytes &data;
    size_t at = 0;
    int budget = 20000;
    void need(size_t n, size_t limit) {
        require(at <= limit && n <= limit - at && limit <= data.size(), "Truncated ValueTree");
    }
    uint32_t count(size_t limit) {
        need(1, limit);
        unsigned n = (unsigned char)data[at++];
        require(n <= 4, "Negative or invalid compressed length");
        need(n, limit);
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) v |= (uint32_t)(unsigned char)data[at++] << (8 * i);
        require(v <= MaxBytes, "ValueTree length exceeds limit");
        return v;
    }
    std::string text() {
        auto end = data.find('\0', at);
        require(end != data.npos && end - at <= 65536 && end > at,
                "Missing terminator or invalid ValueTree identifier");
        auto out = data.substr(at, end - at);
        require(utf8(out), "Invalid UTF-8 identifier");
        for (unsigned char c : out) require(c >= 32, "Invalid ValueTree identifier");
        at = end + 1;
        return out;
    }
    Scalar scalar(const std::string &name, int depth, size_t limit) {
        require(--budget >= 0 && depth <= 64, "ValueTree resource limit");
        Scalar s;
        s.name = name;
        s.start = at;
        auto length = count(limit);
        need(length, limit);
        size_t end = at + length;
        s.end = end;
        if (!length) {
            s.kind = ValueKind::Void;
            return s;
        }
        unsigned marker = (unsigned char)data[at++];
        size_t payload = length - 1;
        if (marker == 1 || marker == 4 || marker == 6) {
            size_t width = marker == 1 ? 4 : 8;
            require(payload == width, "Invalid numeric scalar length");
            uint64_t raw = 0;
            for (size_t i = 0; i < width; ++i)
                raw |= (uint64_t)(unsigned char)data[at + i] << (8 * i);
            if (marker == 4) {
                double value;
                std::memcpy(&value, &raw, 8);
                require(std::isfinite(value), "Non-finite scalar");
                s.number = value;
                s.kind = ValueKind::Double;
            } else if (marker == 1) {
                s.integer = (int32_t)raw;
                s.number = (double)s.integer;
                s.kind = ValueKind::Int;
            } else {
                s.integer = (int64_t)raw;
                s.number = (double)s.integer;
                s.kind = ValueKind::Int64;
            }
            at = end;
        } else if (marker == 2 || marker == 3 || marker == 9) {
            require(!payload, "Invalid boolean/undefined scalar length");
            s.kind = marker == 9 ? ValueKind::Undefined : ValueKind::Bool;
            s.number = marker == 2 ? 1 : 0;
        } else if (marker == 5) {
            require(payload && data[end - 1] == '\0', "Invalid string terminator");
            s.value = data.substr(at, payload - 1);
            require(s.value.find('\0') == s.value.npos && utf8(s.value), "Invalid UTF-8 scalar");
            s.kind = ValueKind::String;
            at = end;
        } else if (marker == 8) {
            s.kind = ValueKind::Binary;
            at = end;
        } else if (marker == 7) {
            auto n = count(end);
            require(n <= (uint32_t)budget, "Array resource limit");
            for (uint32_t i = 0; i < n; ++i) scalar(std::to_string(i), depth + 1, end);
            s.kind = ValueKind::Array;
        } else
            throw Error("Unknown scalar type " + std::to_string(marker));
        require(at == end, "Scalar length mismatch");
        return s;
    }
    Node node(int depth = 0) {
        require(--budget >= 0 && depth <= 64, "ValueTree resource limit");
        Node n;
        n.name = text();
        auto countProps = count(data.size());
        require(countProps <= (uint32_t)budget, "Property resource limit");
        std::set<std::string> names;
        for (uint32_t i = 0; i < countProps; ++i) {
            auto key = text();
            require(names.insert(key).second, "Duplicate property");
            n.props.push_back(scalar(key, depth, data.size()));
        }
        auto countChildren = count(data.size());
        require(countChildren <= (uint32_t)budget, "Child resource limit");
        for (uint32_t i = 0; i < countChildren; ++i) n.children.push_back(node(depth + 1));
        return n;
    }
};
// Parse structure for recognition while retaining source byte spans for exact edits.
struct XmlReader {
    const Bytes &data;
    std::unique_ptr<std::remove_pointer_t<XML_Parser>, decltype(&XML_ParserFree)> parser{
        nullptr, XML_ParserFree};
    std::vector<Node> stack;
    std::optional<Node> root;
    int budget = 20000, width = 1;
    bool big = false;
    std::string error;
    explicit XmlReader(const Bytes &d) : data(d) {
        if (d.size() >= 2 && (((unsigned char)d[0] == 255 && (unsigned char)d[1] == 254) ||
                              (d[0] == '<' && d[1] == 0))) {
            width = 2;
        } else if (d.size() >= 2 && (((unsigned char)d[0] == 254 && (unsigned char)d[1] == 255) ||
                                     (d[0] == 0 && d[1] == '<'))) {
            width = 2;
            big = true;
        }
        parser.reset(XML_ParserCreate(nullptr));
        require(parser != nullptr, "Cannot create XML parser");
        const std::array<uint64_t, 2> salt{entropy(), entropy()};
        require(XML_SetHashSalt16Bytes(parser.get(),
                                      reinterpret_cast<const uint8_t *>(salt.data())),
                "Cannot initialize XML hash salt");
        XML_SetUserData(parser.get(), this);
    }
    std::string encoded(const std::string &s) const { return width == 1 ? s : encode16(s, big); }
    unsigned unit(size_t at) const {
        require(at + width <= data.size(), "Truncated XML tag");
        return width == 1 ? (unsigned char)data[at]
               : big      ? ((unsigned char)data[at] << 8) | (unsigned char)data[at + 1]
                          : (unsigned char)data[at] | ((unsigned char)data[at + 1] << 8);
    }
    template <class Fn> void guard(Fn fn) {
        try {
            fn();
        } catch (const std::exception &e) {
            error = e.what();
            XML_StopParser(parser.get(), XML_FALSE);
        }
    }
    static void XMLCALL start(void *p, const XML_Char *name, const XML_Char **attrs) {
        auto &r = *(XmlReader *)p;
        r.guard([&] {
            require(--r.budget >= 0 && r.stack.size() < 64, "XML resource limit");
            Node n;
            n.name = name;
            size_t at = (size_t)XML_GetCurrentByteIndex64(r.parser.get());
            require(r.unit(at) == '<', "Invalid XML tag offset");
            at += r.width;
            while (at < r.data.size()) {
                auto c = r.unit(at);
                if (c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
                at += r.width;
            }
            size_t attribute = 0;
            for (;;) {
                while (at < r.data.size() && (r.unit(at) == ' ' || r.unit(at) == '\t' ||
                                              r.unit(at) == '\r' || r.unit(at) == '\n'))
                    at += r.width;
                auto c = r.unit(at);
                if (c == '/' || c == '>') break;
                require(attrs[attribute] && attrs[attribute + 1], "Ambiguous XML attributes");
                require(--r.budget >= 0, "XML resource limit");
                auto keyBytes = r.encoded(attrs[attribute]);
                require(r.data.compare(at, keyBytes.size(), keyBytes) == 0,
                        "Ambiguous XML attribute name");
                at += keyBytes.size();
                while (r.unit(at) == ' ' || r.unit(at) == '\t' || r.unit(at) == '\r' ||
                       r.unit(at) == '\n')
                    at += r.width;
                require(r.unit(at) == '=', "Invalid XML attribute span");
                at += r.width;
                while (r.unit(at) == ' ' || r.unit(at) == '\t' || r.unit(at) == '\r' ||
                       r.unit(at) == '\n')
                    at += r.width;
                auto quote = r.unit(at);
                require(quote == '\'' || quote == '"', "Invalid XML quote");
                at += r.width;
                size_t begin = at;
                while (r.unit(at) != quote) at += r.width;
                Scalar s;
                s.name = attrs[attribute];
                s.kind = ValueKind::String;
                s.value = attrs[attribute + 1];
                s.start = begin;
                s.end = at;
                n.props.push_back(std::move(s));
                at += r.width;
                attribute += 2;
            }
            require(!attrs[attribute], "Unmatched XML attributes");
            r.stack.push_back(std::move(n));
        });
    }
    static void XMLCALL end(void *p, const XML_Char *) {
        auto &r = *(XmlReader *)p;
        r.guard([&] {
            require(!r.stack.empty(), "Invalid XML stack");
            auto n = std::move(r.stack.back());
            r.stack.pop_back();
            if (r.stack.empty()) {
                require(!r.root, "Multiple XML roots");
                r.root = std::move(n);
            } else
                r.stack.back().children.push_back(std::move(n));
        });
    }
    static void XMLCALL characters(void *p, const XML_Char *raw, int length) {
        auto &r = *(XmlReader *)p;
        r.guard([&] {
            if (r.stack.empty()) return;
            require(--r.budget >= 0, "XML text event resource limit");
            auto &n = r.stack.back();
            size_t encodedLength = (size_t)length;
            if (r.width == 2) {
                encodedLength = 0;
                require(nd::visitUtf8(
                            std::string_view(raw, (size_t)length),
                            [&](uint32_t point) { encodedLength += point < 0x10000 ? 2 : 4; }),
                        "Invalid XML text encoding");
            }
            size_t begin = (size_t)XML_GetCurrentByteIndex64(r.parser.get()),
                   end = begin + encodedLength;
            if (!n.text) {
                Scalar s;
                s.name = n.name;
                s.kind = ValueKind::String;
                s.start = begin;
                s.end = end;
                n.text = std::move(s);
            }
            n.text->value.append(raw, (size_t)length);
            n.text->end = end;
        });
    }
    static void XMLCALL doctype(void *p, const XML_Char *, const XML_Char *, const XML_Char *,
                                int) {
        auto &r = *(XmlReader *)p;
        r.error = "DTD and entity declarations are forbidden";
        XML_StopParser(r.parser.get(), XML_FALSE);
    }
    static void XMLCALL declaration(void *p, const XML_Char *, const XML_Char *encoding, int) {
        auto &r = *(XmlReader *)p;
        if (!encoding) return;
        auto name = lower(encoding);
        std::replace(name.begin(), name.end(), '_', '-');
        if (name != "utf-8" && name != "utf-16" && name != "utf-16le" && name != "utf-16be") {
            r.error = "Only UTF-8/UTF-16 XML encodings are supported";
            XML_StopParser(r.parser.get(), XML_FALSE);
        }
    }
    Node parse() {
        XML_SetElementHandler(parser.get(), start, end);
        XML_SetCharacterDataHandler(parser.get(), characters);
        XML_SetStartDoctypeDeclHandler(parser.get(), doctype);
        XML_SetXmlDeclHandler(parser.get(), declaration);
        auto result = XML_Parse(parser.get(), data.data(), (int)data.size(), XML_TRUE);
        require(result == XML_STATUS_OK,
                error.empty() ? std::string("Malformed or unsupported text XML: ") +
                                    (XML_GetErrorCode(parser.get()) == XML_ERROR_NONE
                                         ? "none"
                                         : XML_ErrorString(XML_GetErrorCode(parser.get())))
                              : error);
        require(root.has_value(), "Empty XML");
        return std::move(*root);
    }
};
static bool looksXml(const Bytes &d) {
    if (d.size() >= 2 && (((unsigned char)d[0] == 255 && (unsigned char)d[1] == 254) ||
                          ((unsigned char)d[0] == 254 && (unsigned char)d[1] == 255) ||
                          (d[0] == '<' && d[1] == 0) || (d[0] == 0 && d[1] == '<')))
        return true;
    size_t at = d.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
    while (at < d.size() && (d[at] == ' ' || d[at] == '\r' || d[at] == '\n' || d[at] == '\t')) ++at;
    return at < d.size() && d[at] == '<';
}
static bool cabKey(const std::string &s) { return s == "cabActive" || s == "cabSectionActive"; }
static bool paramKey(const std::string &s) { return cabKey(s) || s == "cabActiveID" || s == "cabsim_bypass"; }
// Numeric IDs are shared by the legacy two-slot cab section. Require its
// explicit IR container and both microphone parameter groups, never ID 400 alone.
static bool numericCabSection(const Node &n) {
    bool irs = false;
    unsigned containers = 0;
    for (const auto &c : n.children) {
        if (c.name == "cabsims") ++containers;
        if (c.name == "cabsims" && c.props.empty() && !c.children.empty()) {
            irs = !c.text || trim(c.text->value).empty();
            for (const auto &slot : c.children)
                irs = irs && slot.name == "cabsim" && slot.children.empty() &&
                      slot.findProperty("filePath1") && slot.findProperty("filePath2") &&
                      slot.property("filePath1").kind == ValueKind::String &&
                      slot.property("filePath2").kind == ValueKind::String &&
                      (!slot.text || trim(slot.text->value).empty());
        }
    }
    if (!irs || containers != 1) return false;
    std::set<std::string> ids;
    for (const auto &c : n.children)
        if (c.name == "PARAM") ids.insert(c.property("id").value);
    for (auto id : {"400", "403", "404", "405", "406", "407", "408", "409",
                    "411", "412", "415", "416", "417", "418", "419", "420",
                    "421", "423", "424"})
        if (!ids.count(id)) return false;
    return irs;
}
static void candidates(const Node &n, const std::string &prefix, bool root,
                       std::vector<std::pair<const Scalar *, std::string>> &found) {
    auto path = prefix + "/" + n.name;
    std::set<std::string> ids;
    bool numeric = numericCabSection(n);
    for (auto &c : n.children) {
        if (c.name == "PARAM") {
            auto &id = c.property("id");
            require(id.kind == ValueKind::String && !id.value.empty() &&
                        ids.insert(id.value).second,
                    "Duplicate or invalid PARAM identity");
        }
        if (numeric && c.name == "PARAM" && c.property("id").value == "400") {
            require(c.children.empty() && c.props.size() == 2 &&
                        c.findProperty("value") && (!c.text || trim(c.text->value).empty()),
                    "Ambiguous numeric whole-cab PARAM structure");
            found.emplace_back(&c.property("value"), path + "/PARAM[400]");
        }
        candidates(c, path, false, found);
    }
    if (n.name == "PARAM") {
        auto &id = n.property("id");
        if (paramKey(id.value)) {
            bool attributes = n.children.empty() && n.props.size() == 2;
            std::set<std::string> p;
            for (auto &prop : n.props) p.insert(prop.name);
            attributes = attributes && p == std::set<std::string>{"id", "value"};
            bool elements = n.props.empty() && n.children.size() == 2;
            std::set<std::string> children;
            for (auto &c : n.children) {
                elements = elements && c.children.empty() && c.props.empty();
                children.insert(c.name);
            }
            elements = elements && children == std::set<std::string>{"id", "value"};
            require((attributes || elements) && (!n.text || trim(n.text->value).empty()),
                    "Ambiguous whole-cab PARAM structure");
            found.emplace_back(&n.property("value"), path + "[" + id.value + "]");
        }
    } else
        for (auto &p : n.props) {
            bool global = cabKey(p.name) && (root || n.name == "parameters" ||
                                             n.name == "appModel" || n.name == "cabParameters");
            bool section = n.name == "cabParameters" && p.name == "sectionActive";
            // Legacy section switches use 1 for active despite the bypass name.
            bool legacy = n.name == "Parameter" && p.name == "cabsim_bypass";
            if (global || section || legacy) {
                require(!n.text || trim(n.text->value).empty(),
                        "Whole-cab property mixed with ambiguous text");
                found.emplace_back(&p, path + "@" + p.name);
            }
        }
}
static Bytes compressed(uint32_t value) {
    Bytes out;
    uint32_t n = value;
    unsigned width = 0;
    while (n) {
        ++width;
        n >>= 8;
    }
    out += char(width);
    for (unsigned i = 0; i < width; ++i) out += char(value >> (i * 8));
    return out;
}
static std::string metadata(const Node &root, std::initializer_list<const char *> keys) {
    for (auto *key : keys) try {
            const auto *p = root.findProperty(key);
            if (!p) continue;
            if (p->kind == ValueKind::String) return p->value;
            if (p->kind == ValueKind::Int || p->kind == ValueKind::Int64)
                return std::to_string(p->integer);
            if (p->kind == ValueKind::Double) return std::to_string(p->number);
        } catch (const Error &) {
        }
    return "";
}
Inspection inspect(const Bytes &data) {
    require(data.size() <= MaxBytes, "Preset exceeds 4 MiB safety limit");
    bool xml = looksXml(data);
    Node root;
    std::unique_ptr<XmlReader> reader;
    if (xml) {
        reader = std::make_unique<XmlReader>(data);
        root = reader->parse();
    } else {
        BinaryReader r{data};
        root = r.node();
        require(r.at == data.size(), "Trailing bytes after ValueTree");
    }
    std::vector<std::pair<const Scalar *, std::string>> found;
    // Collect every eligible whole-cab switch; multiple matches are rejected below.
    candidates(root, "", true, found);
    require(found.size() == 1,
            "Expected one unambiguous whole-cab flag; microphone/IR switches are excluded");
    auto &s = *found[0].first;
    Inspection info;
    info.format = xml ? "text.xml" : "binary.valuetree";
    info.product = root.name;
    info.version = metadata(root, {"version", "presetVersionProp"});
    if (info.version.empty()) info.version = "not recorded";
    info.display = metadata(root, {"name", "presetNameProp"});
    static constexpr const char *kindNames[] = {"void",   "bool",      "int",    "int64", "double",
                                                "string", "undefined", "binary", "array"};
    info.schema = found[0].second + ":" + kindNames[static_cast<size_t>(s.kind)];
    info.start = s.start;
    info.end = s.end;
    require(info.end > info.start && info.end <= data.size(), "Invalid cab scalar span");
    auto raw = data.substr(s.start, s.end - s.start);
    if (s.kind == ValueKind::String) {
        std::string off, on;
        for (auto pair : {std::pair<const char *, const char *>{"false", "true"},
                          {"False", "True"},
                          {"FALSE", "TRUE"}})
            if (s.value == pair.first || s.value == pair.second) {
                off = pair.first;
                on = pair.second;
                break;
            }
        if (off.empty()) {
            require(!s.value.empty() && (s.value[0] == '0' || s.value[0] == '1'),
                    "Whole-cab flag must be true/false or 0/1 scalar");
            require(s.value.size() == 1 || (s.value.size() > 2 && s.value[1] == '.' &&
                                            s.value.find_first_not_of('0', 2) == s.value.npos),
                    "Whole-cab flag must be true/false or 0/1 scalar");
            off = "0" + s.value.substr(1);
            on = "1" + s.value.substr(1);
        }
        info.enabled = s.value == on;
        if (xml) {
            require(raw == reader->encoded(s.value),
                    "Whole-cab value uses entities or normalized whitespace");
            info.off = reader->encoded(off);
            info.on = reader->encoded(on);
        } else {
            auto encode = [](const std::string &value) {
                Bytes payload(1, 5);
                payload += value;
                payload += '\0';
                return compressed((uint32_t)payload.size()) + payload;
            };
            info.off = encode(off);
            info.on = encode(on);
        }
    } else if (s.kind == ValueKind::Bool) {
        info.enabled = s.number == 1;
        info.off = raw;
        info.on = raw;
        info.off.back() = 3;
        info.on.back() = 2;
    } else {
        require((s.kind == ValueKind::Int || s.kind == ValueKind::Int64 ||
                 s.kind == ValueKind::Double) &&
                    (s.number == 0 || s.number == 1),
                "Unsupported whole-cab value type or polarity");
        info.enabled = s.number == 1;
        size_t width = s.kind == ValueKind::Int ? 4 : 8;
        info.off = raw;
        info.on = raw;
        uint64_t bits = 1;
        if (s.kind == ValueKind::Double) {
            double v = 1;
            std::memcpy(&bits, &v, 8);
        }
        for (size_t i = 0; i < width; ++i) {
            info.off[raw.size() - width + i] = 0;
            info.on[raw.size() - width + i] = (char)(bits >> (8 * i));
        }
    }
    return info;
}
// Splice the selected scalar into the original bytes instead of serializing the tree.
Bytes patch(const Bytes &data, bool enabled) {
    auto before = inspect(data);
    if (before.enabled == enabled) return data;
    const auto &value = enabled ? before.on : before.off;
    Bytes output;
    output.reserve(data.size() - (before.end - before.start) + value.size());
    output.append(data, 0, before.start).append(value).append(data, before.end, data.npos);
    auto after = inspect(output);
    require(after.enabled == enabled && before.format == after.format &&
                before.product == after.product && before.version == after.version &&
                before.schema == after.schema && before.display == after.display,
            "Patched cab state/metadata did not validate");
    require(before.start == after.start &&
                data.compare(0, before.start, output, 0, after.start) == 0 &&
                data.compare(before.end, data.npos, output, after.end, output.npos) == 0,
            "Changed bytes outside validated cab scalar");
    return output;
}
std::string representation(const Bytes &data) {
    if (data.size() > MaxBytes) return "oversized";
    if (!looksXml(data)) return "binary / unknown (possibly encrypted or compressed)";
    try {
        XmlReader r(data);
        r.parse();
        return "text XML (schema unverified)";
    } catch (...) {
        return "invalid / unsafe XML";
    }
}
} // namespace cab
