#include <TextUtf8.h>

#include <array>
#include <cstdlib>

#include <utf8proc.h>

namespace NarrativeEngine::TextUtf8
{
    namespace
    {
        constexpr char32_t kReplacement = 0xFFFD;

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

        [[nodiscard]] bool IsTrimmable(char32_t codePoint) noexcept
        {
            return codePoint == U'-' || codePoint == U'\'';
        }

        // The code point that ends just before byte `at`, or nothing when
        // `at` is the start. A malformed byte reads as U+FFFD.
        [[nodiscard]] char32_t CodePointBefore(std::string_view text, std::size_t at) noexcept
        {
            // A sequence is at most four bytes, so its lead is at most three
            // continuation bytes back.
            std::size_t start = at - 1;
            for (std::size_t back = 0; back < 3 && start > 0 && IsContinuation(static_cast<unsigned char>(text[start]));
                 ++back) {
                --start;
            }
            const auto decoded = DecodeAt(text, start);
            return start + decoded.length == at ? decoded.codePoint : kReplacement;
        }

        // Apply a one-to-one code point mapping to the first `limit` code
        // points, copying malformed bytes and everything after `limit`
        // through untouched.
        template <typename Map>
        [[nodiscard]] std::string MapCodePoints(std::string_view text, std::size_t limit, Map map)
        {
            std::string out;
            out.reserve(text.size());
            std::size_t at = 0;
            std::size_t mapped = 0;
            while (at < text.size()) {
                const auto length = SequenceLength(text, at);
                if (length == 0) {
                    out.push_back(text[at]);
                    ++at;
                    continue;
                }
                if (mapped < limit) {
                    AppendCodePoint(out, map(DecodeAt(text, at).codePoint));
                    ++mapped;
                } else {
                    out.append(text.substr(at, length));
                }
                at += length;
            }
            return out;
        }
    } // namespace

    std::size_t SequenceLength(std::string_view text, std::size_t at) noexcept
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

    Decoded DecodeAt(std::string_view text, std::size_t at) noexcept
    {
        const auto length = SequenceLength(text, at);
        if (length == 0) {
            return {kReplacement, 1};
        }
        const auto lead = static_cast<unsigned char>(text[at]);
        if (length == 1) {
            return {lead, 1};
        }
        constexpr std::array<unsigned char, 5> kLeadMask = {0, 0, 0x1F, 0x0F, 0x07};
        char32_t codePoint = lead & kLeadMask[length];
        for (std::size_t i = 1; i < length; ++i) {
            codePoint = (codePoint << 6) | (static_cast<unsigned char>(text[at + i]) & 0x3F);
        }
        return {codePoint, length};
    }

    bool IsValid(std::string_view text) noexcept
    {
        std::size_t at = 0;
        while (at < text.size()) {
            const auto length = SequenceLength(text, at);
            if (length == 0) {
                return false;
            }
            at += length;
        }
        return true;
    }

    void AppendCodePoint(std::string& out, char32_t codePoint)
    {
        if (codePoint < 0x80) {
            out.push_back(static_cast<char>(codePoint));
        } else if (codePoint < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
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
            AppendCodePoint(out, byte >= 0x80 && byte <= 0x9F ? kWindows1252High[byte - 0x80] : char32_t{byte});
            repaired = true;
            ++at;
        }
        return repaired ? out : std::string{text};
    }

    std::size_t CodePointCount(std::string_view text) noexcept
    {
        std::size_t count = 0;
        for (std::size_t at = 0; at < text.size(); at += DecodeAt(text, at).length) {
            ++count;
        }
        return count;
    }

    void TruncateToCodePoints(std::string& text, std::size_t maxCodePoints)
    {
        std::size_t at = 0;
        for (std::size_t kept = 0; at < text.size() && kept < maxCodePoints; ++kept) {
            at += DecodeAt(text, at).length;
        }
        text.resize(at);
    }

    std::string ToLower(std::string_view text)
    {
        return MapCodePoints(text, text.size(), [](char32_t c) {
            return static_cast<char32_t>(utf8proc_tolower(static_cast<utf8proc_int32_t>(c)));
        });
    }

    std::string ToUpper(std::string_view text)
    {
        return MapCodePoints(text, text.size(), [](char32_t c) {
            return static_cast<char32_t>(utf8proc_toupper(static_cast<utf8proc_int32_t>(c)));
        });
    }

    std::string UppercaseFirst(std::string_view text)
    {
        return MapCodePoints(text, 1, [](char32_t c) {
            return static_cast<char32_t>(utf8proc_toupper(static_cast<utf8proc_int32_t>(c)));
        });
    }

    std::string LowercaseFirst(std::string_view text)
    {
        return MapCodePoints(text, 1, [](char32_t c) {
            return static_cast<char32_t>(utf8proc_tolower(static_cast<utf8proc_int32_t>(c)));
        });
    }

    std::string Fold(std::string_view text)
    {
        // utf8proc refuses malformed input outright, so malformed bytes are
        // replaced with U+FFFD first, and all fold to that.
        std::string clean;
        clean.reserve(text.size());
        for (std::size_t at = 0; at < text.size();) {
            const auto decoded = DecodeAt(text, at);
            AppendCodePoint(clean, decoded.codePoint);
            at += decoded.length;
        }

        utf8proc_uint8_t* folded = nullptr;
        const auto length =
            utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(clean.data()),
                         static_cast<utf8proc_ssize_t>(clean.size()),
                         &folded,
                         static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD));
        if (length < 0 || folded == nullptr) {
            std::free(folded);
            return ToLower(clean);
        }
        std::string out(reinterpret_cast<const char*>(folded), static_cast<std::size_t>(length));
        std::free(folded);
        return out;
    }

    bool EqualsFolded(std::string_view a, std::string_view b)
    {
        return Fold(a) == Fold(b);
    }

    bool IsLetter(char32_t codePoint) noexcept
    {
        switch (utf8proc_category(static_cast<utf8proc_int32_t>(codePoint))) {
        case UTF8PROC_CATEGORY_LU:
        case UTF8PROC_CATEGORY_LL:
        case UTF8PROC_CATEGORY_LT:
        case UTF8PROC_CATEGORY_LM:
        case UTF8PROC_CATEGORY_LO:
            return true;
        default:
            return false;
        }
    }

    bool IsUpper(char32_t codePoint) noexcept
    {
        return utf8proc_isupper(static_cast<utf8proc_int32_t>(codePoint)) != 0;
    }

    bool IsLower(char32_t codePoint) noexcept
    {
        return utf8proc_islower(static_cast<utf8proc_int32_t>(codePoint)) != 0;
    }

    bool IsWordChar(char32_t codePoint) noexcept
    {
        if (codePoint == kReplacement) {
            return false;
        }
        if (IsTrimmable(codePoint) || IsLetter(codePoint)) {
            return true;
        }
        switch (utf8proc_category(static_cast<utf8proc_int32_t>(codePoint))) {
        case UTF8PROC_CATEGORY_MN:
        case UTF8PROC_CATEGORY_MC:
        case UTF8PROC_CATEGORY_ME:
        case UTF8PROC_CATEGORY_ND:
            return true;
        default:
            return false;
        }
    }

    std::vector<std::string_view> Words(std::string_view text)
    {
        std::vector<std::string_view> out;
        std::size_t start = 0;
        std::size_t at = 0;
        const auto flush = [&](std::size_t end) {
            // Trim hyphens and apostrophes off both ends; both are one byte.
            while (start < end && IsTrimmable(static_cast<unsigned char>(text[start]))) {
                ++start;
            }
            while (end > start && IsTrimmable(static_cast<unsigned char>(text[end - 1]))) {
                --end;
            }
            if (end > start) {
                out.push_back(text.substr(start, end - start));
            }
        };
        while (at < text.size()) {
            const auto decoded = DecodeAt(text, at);
            if (!IsWordChar(decoded.codePoint)) {
                flush(at);
                start = at + decoded.length;
            }
            at += decoded.length;
        }
        flush(text.size());
        return out;
    }

    bool StandsAlone(std::string_view text, std::size_t at, std::size_t length, TrailingApostrophe trailing) noexcept
    {
        if (at > 0 && IsWordChar(CodePointBefore(text, at))) {
            return false;
        }
        const auto after = at + length;
        if (after >= text.size()) {
            return true;
        }
        const auto next = DecodeAt(text, after).codePoint;
        if (next == U'\'' && trailing == TrailingApostrophe::EndsWord) {
            return true;
        }
        return !IsWordChar(next);
    }
} // namespace NarrativeEngine::TextUtf8
