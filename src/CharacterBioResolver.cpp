#include <CharacterBioResolver.h>

#include <array>
#include <cstddef>
#include <format>

namespace NarrativeEngine::CharacterBioResolver
{
    namespace
    {
        constexpr std::string_view kWhitespace = " \t\n\r\f\v";

        // Windows-1252's 0x80-0x9F, as SkyrimNet's table has them. The five
        // bytes the code page leaves undefined map to the C1 control of the
        // same value, as they do there.
        constexpr std::array<char32_t, 32> kWindows1252High = {
            0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
            0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
            0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

        [[nodiscard]] bool IsContinuation(unsigned char c) noexcept
        {
            return (c & 0xC0) == 0x80;
        }

        // The length of the well-formed UTF-8 sequence starting at `at`, or 0
        // when it is not one. The ranges are Unicode's table of well-formed
        // sequences: no overlongs, no surrogates, nothing past U+10FFFF.
        [[nodiscard]] std::size_t SequenceLength(std::string_view text, std::size_t at) noexcept
        {
            const auto lead = static_cast<unsigned char>(text[at]);
            if (lead < 0x80) {
                return 1;
            }
            std::size_t length = 0;
            unsigned char low = 0x80;
            unsigned char high = 0xBF;
            if (lead >= 0xC2 && lead <= 0xDF) {
                length = 2;
            } else if (lead >= 0xE0 && lead <= 0xEF) {
                length = 3;
                low = lead == 0xE0 ? 0xA0 : 0x80;
                high = lead == 0xED ? 0x9F : 0xBF;
            } else if (lead >= 0xF0 && lead <= 0xF4) {
                length = 4;
                low = lead == 0xF0 ? 0x90 : 0x80;
                high = lead == 0xF4 ? 0x8F : 0xBF;
            } else {
                return 0;
            }
            if (at + length > text.size()) {
                return 0;
            }
            const auto second = static_cast<unsigned char>(text[at + 1]);
            if (second < low || second > high) {
                return 0;
            }
            for (std::size_t i = 2; i < length; ++i) {
                if (!IsContinuation(static_cast<unsigned char>(text[at + i]))) {
                    return 0;
                }
            }
            return length;
        }

        void AppendUtf8(std::string& out, char32_t cp)
        {
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        [[nodiscard]] char AsciiLower(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        [[nodiscard]] std::string Suffixed(std::string_view stem, std::uint32_t refId)
        {
            return std::format("{}_{:03X}", stem, refId & 0xFFF);
        }
    } // namespace

    std::uint32_t LocalId(std::uint32_t formId) noexcept
    {
        return (formId & 0xFF000000) == 0xFE000000 ? formId & 0xFFF : formId & 0xFFFFFF;
    }

    std::string UniqueKey(std::string_view plugin, std::uint32_t localId)
    {
        std::string key;
        for (const char c : Trim(plugin)) {
            key.push_back(AsciiLower(c));
        }
        // The separator is ours, not SkyrimNet's: it only has to keep the two
        // parts from running together, and no file name carries a '|'.
        key += std::format("|{:06X}", localId);
        return key;
    }

    std::string_view Trim(std::string_view text) noexcept
    {
        const auto first = text.find_first_not_of(kWhitespace);
        if (first == std::string_view::npos) {
            return {};
        }
        const auto last = text.find_last_not_of(kWhitespace);
        return text.substr(first, last - first + 1);
    }

    std::string Slug(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (const char c : text) {
            out.push_back(c == ' ' || c == '\'' || c == '\\' || c == '/' ? '_' : AsciiLower(c));
        }
        return out;
    }

    std::string RepairWindows1252(std::string_view text)
    {
        std::string out;
        bool repaired = false;
        std::size_t at = 0;
        while (at < text.size()) {
            if (const auto length = SequenceLength(text, at); length != 0) {
                out.append(text.substr(at, length));
                at += length;
                continue;
            }
            const auto byte = static_cast<unsigned char>(text[at]);
            AppendUtf8(out, byte >= 0x80 && byte <= 0x9F ? kWindows1252High[byte - 0x80] : char32_t{byte});
            repaired = true;
            ++at;
        }
        return repaired ? out : std::string{text};
    }

    std::string BioName(const ActorFacts& facts, const Tables& tables)
    {
        // 1. The unique table, by the base NPC's plugin and the reference's
        //    own local id. Never for the player or a null id.
        if (facts.refId != 0 && facts.refId != kPlayerRefId) {
            const auto& plugin = facts.basePlugin.empty() ? facts.refPlugin : facts.basePlugin;
            if (!plugin.empty()) {
                if (const auto hit = tables.unique.find(UniqueKey(plugin, LocalId(facts.refId)));
                    hit != tables.unique.end()) {
                    return Suffixed(hit->second, facts.refId);
                }
            }
        }

        const std::string_view raw = !facts.displayName.empty() ? std::string_view{facts.displayName}
                                     : !facts.baseName.empty()  ? std::string_view{facts.baseName}
                                                                : std::string_view{"Unknown"};
        const auto name = RepairWindows1252(raw);

        // 2. The generic table, on the trimmed name.
        if (const auto trimmed = Trim(name); !trimmed.empty()) {
            if (const auto hit = tables.generic.find(Slug(trimmed)); hit != tables.generic.end()) {
                return Suffixed(hit->second, facts.refId);
            }
        }

        // 3. The fallback: the player, no usable name, or the slug.
        if (facts.refId == kPlayerRefId) {
            return "player_special";
        }
        if (name.empty() || name == "Unknown") {
            return {};
        }
        return Suffixed(Slug(name), facts.refId);
    }

} // namespace NarrativeEngine::CharacterBioResolver
