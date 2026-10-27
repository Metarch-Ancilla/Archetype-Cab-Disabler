// Synthetic section layouts only; never factory preset contents.
#include "../backend/core.hpp"
#include <iostream>
using namespace cab;
static void reject(const Bytes &data) {
    bool failed = false;
    try {
        inspect(data);
    } catch (const Error &) {
        failed = true;
    }
    require(failed, "Ambiguous/unsupported cab section accepted");
}
static void roundtrip(const Bytes &data) {
    auto original = inspect(data);
    for (bool enabled : {false, true}) {
        auto output = patch(data, enabled);
        auto result = inspect(output);
        require(result.enabled == enabled && result.schema == original.schema,
                "Cab state/schema mismatch");
        require(data.substr(0, original.start) == output.substr(0, result.start) &&
                    data.substr(original.end) == output.substr(result.end),
                "Changed bytes outside whole-cab scalar");
        require(patch(output, enabled) == output, "Cab edit not idempotent");
        require(patch(output, original.enabled) == data, "Cab edit not reversible");
    }
}
static std::string numeric(const std::string &extra = "", const std::string &skip = "") {
    std::string out = "<future>";
    for (auto id : {"400", "403", "404", "405", "406", "407", "408", "409", "411", "412", "415",
                    "416", "417", "418", "419", "420", "421", "423", "424"})
        if (id != skip) out += "<PARAM id=\"" + std::string(id) + "\" value=\"1.000\"/>";
    return out +
           "<cabsims><cabsim filePath1=\"one.wav\" filePath2=\"two.wav\"/>"
           "</cabsims>" +
           extra + "</future>";
}
// Minimal generated JUCE serializer for independent binary regression cases.
static Bytes stringValue(const std::string &value) {
    return Bytes({1, static_cast<char>(value.size() + 2), 5}) + value + '\0';
}
static Bytes node(const std::string &name, const std::vector<std::pair<std::string, Bytes>> &props,
                  const std::vector<Bytes> &children = {}) {
    Bytes out = name + '\0';
    out += props.empty() ? Bytes(1, 0) : Bytes({1, static_cast<char>(props.size())});
    for (const auto &p : props) out += p.first + '\0' + p.second;
    out += children.empty() ? Bytes(1, 0) : Bytes({1, static_cast<char>(children.size())});
    for (const auto &c : children) out += c;
    return out;
}
int main(int argc, char **) {
    try {
        require(argc == 2, "Provide test directory");
        for (auto state : {"0.00000000000000000000", "1.00000000000000000000"}) {
            roundtrip("<future><Parameter cabsim_bypass=\"" + std::string(state) +
                      "\" cabsim1_enabled=\"1\" cabsim2_enabled=\"0\"/></future>");
            roundtrip("<future><PARAM id=\"cabsim_bypass\" value=\"" + std::string(state) +
                      "\"/><PARAM id=\"cabsim1_enabled\" value=\"1\"/></future>");
        }
        // Malformed UTF-16 must be rejected even outside the cab scalar.
        for (bool big : {false, true}) {
            const Bytes prefix = encode16("<future cabActive=\"1\"><note>", big);
            const Bytes suffix = encode16("</note></future>", big);
            roundtrip(prefix + encode16(u8"🎸", big) + suffix);
            for (const Bytes &bad : {big ? Bytes("\xd8\0\0A", 4) : Bytes("\0\xd8" "A\0", 4),
                                     big ? Bytes("\xdc\0", 2) : Bytes("\0\xdc", 2)})
                reject(prefix + bad + suffix);
        }
        auto param = [](const std::string &id, const std::string &value) {
            return node("PARAM", {{"id", stringValue(id)}, {"value", stringValue(value)}});
        };
        roundtrip(
            node("future", {}, {param("cabsim_bypass", "1.000"), param("cabsim1_enabled", "0")}));
        std::vector<Bytes> children;
        for (auto id : {"400", "403", "404", "405", "406", "407", "408", "409", "411", "412", "415",
                        "416", "417", "418", "419", "420", "421", "423", "424"})
            children.push_back(param(id, "1.0"));
        children.push_back(node("cabsims", {},
                                {node("cabsim", {{"filePath1", stringValue("a.wav")},
                                                 {"filePath2", stringValue("b.wav")}})}));
        roundtrip(node("unseen_product", {}, children));
        children.push_back(param("cabActive", "true"));
        reject(node("unseen_product", {}, children));
        roundtrip(numeric());
        roundtrip(numeric("<PARAM id=\"401\" value=\"2\"/>"));
        reject("<future><PARAM id=\"400\" value=\"1\"/></future>");
        reject(numeric("", "403"));
        reject(numeric("", "415"));
        reject(numeric("<PARAM id=\"400\" value=\"1\"/>"));
        reject(numeric("<PARAM id=\"cabActive\" value=\"1\"/>"));
        reject(numeric("<cabsims><cabsim filePath1=\"a\" filePath2=\"b\"/></cabsims>"));
        reject("<future cabActive=\"true\"><Parameter cabsim_bypass=\"1\"/></future>");
        reject("<future><Parameter cabsim_bypass=\"0.5\"/></future>");
        reject("<future><Parameter cabsim1_enabled=\"1\" cabsim2_enabled=\"1\"/></future>");
        reject("<future cabsim_bypass=\"1\"/>");
        std::cout
            << "Generalized legacy sections, flag-only roundtrips and ambiguity checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
