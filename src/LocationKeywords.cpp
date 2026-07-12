#include <LocationKeywords.h>

#include <logger.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace NarrativeEngine::LocationKeywords
{
    namespace
    {
        // Upper bound on parentLoc traversal depth — vanilla never nests
        // more than two or three deep, but cycles are theoretically
        // possible if mod data is malformed, so cap defensively.
        constexpr int kMaxParentDepth = 16;

        RE::BGSKeyword* LookupKeyword(std::string_view edid)
        {
            auto* form = RE::TESForm::LookupByEditorID(edid);
            if (!form) {
                logger::warn(
                    "LocationKeywords: keyword EditorID '{}' did not resolve; filter degraded (is powerofthree's Tweaks installed?)",
                    edid);
                return nullptr;
            }
            auto* kw = form->As<RE::BGSKeyword>();
            if (!kw) {
                logger::warn(
                    "LocationKeywords: EditorID '{}' resolved to a non-keyword form (type=0x{:02X}); filter degraded",
                    edid,
                    static_cast<unsigned>(form->GetFormType()));
                return nullptr;
            }
            return kw;
        }

        const std::array<RE::BGSKeyword*, kSafe.size()>& ResolvedSafe()
        {
            static const auto cache = []() {
                std::array<RE::BGSKeyword*, kSafe.size()> out{};
                for (std::size_t i = 0; i < kSafe.size(); ++i) {
                    out[i] = LookupKeyword(kSafe[i]);
                }
                return out;
            }();
            return cache;
        }

        const std::array<RE::BGSKeyword*, kDangerous.size()>& ResolvedDangerous()
        {
            static const auto cache = []() {
                std::array<RE::BGSKeyword*, kDangerous.size()> out{};
                for (std::size_t i = 0; i < kDangerous.size(); ++i) {
                    out[i] = LookupKeyword(kDangerous[i]);
                }
                return out;
            }();
            return cache;
        }

        const std::array<RE::BGSKeyword*, kVisitHostileExtras.size()>& ResolvedVisitHostileExtras()
        {
            static const auto cache = []() {
                std::array<RE::BGSKeyword*, kVisitHostileExtras.size()> out{};
                for (std::size_t i = 0; i < kVisitHostileExtras.size(); ++i) {
                    out[i] = LookupKeyword(kVisitHostileExtras[i]);
                }
                return out;
            }();
            return cache;
        }

        // Best-effort human-readable label for a Location: prefer the
        // display name (FULL record), fall back to EditorID (requires
        // powerofthree's Tweaks for runtime retention), then a FormID hex
        // string if both are unavailable.
        std::string LocationLabel(RE::BGSLocation* loc)
        {
            if (!loc) {
                return "(null)";
            }
            if (const char* full = loc->GetFullName(); full && *full) {
                return std::string(full);
            }
            if (const char* edid = loc->GetFormEditorID(); edid && *edid) {
                return std::string(edid);
            }
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%08X", loc->GetFormID());
            return std::string(buf);
        }

        // Render the walked chain (start → ... → matched) as
        // "A -> B -> C".
        std::string FormatChain(const std::vector<RE::BGSLocation*>& chain)
        {
            std::string out;
            for (auto* l : chain) {
                if (!out.empty())
                    out += " -> ";
                out += LocationLabel(l);
            }
            return out;
        }

        template <std::size_t N>
        bool LocationOrAncestorHasAny(RE::BGSLocation* loc,
                                      const std::array<RE::BGSKeyword*, N>& keywords,
                                      std::string_view setLabel)
        {
            if (!loc) {
                return false;
            }
            RE::BGSLocation* const startLoc = loc;
            std::vector<RE::BGSLocation*> chain;
            chain.reserve(kMaxParentDepth);
            for (int depth = 0; loc && depth < kMaxParentDepth; ++depth) {
                chain.push_back(loc);
                for (auto* kw : keywords) {
                    if (kw && loc->HasKeyword(kw)) {
                        const char* kwEdid = kw->GetFormEditorID();
                        logger::debug("LocationKeywords: {} is {} because keyword {} is on {}",
                                      LocationLabel(startLoc),
                                      setLabel,
                                      (kwEdid && *kwEdid) ? kwEdid : "(unknown)",
                                      FormatChain(chain));
                        return true;
                    }
                }
                loc = loc->parentLoc;
            }
            return false;
        }
    } // namespace

    bool IsSafe(RE::BGSLocation* loc)
    {
        return LocationOrAncestorHasAny(loc, ResolvedSafe(), "Safe");
    }

    bool IsDangerous(RE::BGSLocation* loc)
    {
        return LocationOrAncestorHasAny(loc, ResolvedDangerous(), "Dangerous");
    }

    bool IsVisitHostile(RE::BGSLocation* loc)
    {
        if (LocationOrAncestorHasAny(loc, ResolvedDangerous(), "VisitHostile(dangerous)")) {
            return true;
        }
        return LocationOrAncestorHasAny(loc, ResolvedVisitHostileExtras(), "VisitHostile(extras)");
    }
} // namespace NarrativeEngine::LocationKeywords
