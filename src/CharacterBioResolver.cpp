#include <CharacterBioResolver.h>
#include <TextUtf8.h>

#include <algorithm>
#include <cstddef>
#include <format>

#include <nlohmann/json.hpp>

namespace NarrativeEngine::CharacterBioResolver
{
    namespace
    {
        constexpr std::string_view kWhitespace = " \t\n\r\f\v";

        [[nodiscard]] char AsciiLower(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        [[nodiscard]] std::string Suffixed(std::string_view stem, std::uint32_t refId)
        {
            return std::format("{}_{:03X}", stem, refId & 0xFFF);
        }

        [[nodiscard]] bool IsHexDigit(char c) noexcept
        {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        // One CSV line's cells, the way SkyrimNet's reader (rapidcsv, with
        // auto-unquoting and no quoted line breaks) splits them: ',' outside
        // quotes separates, a quoted cell loses its quotes and "" inside it
        // is one quote. Cells are not trimmed here; the callers trim.
        [[nodiscard]] std::vector<std::string> SplitCsvLine(std::string_view line)
        {
            std::vector<std::string> cells(1);
            bool quoted = false;
            for (std::size_t i = 0; i < line.size(); ++i) {
                const char c = line[i];
                if (quoted) {
                    if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                        cells.back().push_back('"');
                        ++i;
                    } else if (c == '"') {
                        quoted = false;
                    } else {
                        cells.back().push_back(c);
                    }
                } else if (c == '"') {
                    quoted = true;
                } else if (c == ',') {
                    cells.emplace_back();
                } else {
                    cells.back().push_back(c);
                }
            }
            return cells;
        }

        // Every data row of a file: the header line dropped, a UTF-8 byte
        // order mark and each line's trailing CR removed.
        template <typename RowFn> void ForEachRow(std::string_view text, RowFn&& onRow)
        {
            constexpr std::string_view kBom = "\xEF\xBB\xBF";
            if (text.starts_with(kBom)) {
                text.remove_prefix(kBom.size());
            }
            bool header = true;
            while (!text.empty()) {
                const auto end = text.find('\n');
                auto line = text.substr(0, end);
                text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
                if (line.ends_with('\r')) {
                    line.remove_suffix(1);
                }
                if (header) {
                    header = false;
                    continue;
                }
                onRow(SplitCsvLine(line));
            }
        }

        // The three cells every row of either table carries, trimmed, or
        // nothing when the row is one SkyrimNet skips.
        struct Row
        {
            std::string plugin;
            std::uint32_t id = 0;
            std::string key;
        };

        [[nodiscard]] std::optional<Row> ReadRow(const std::vector<std::string>& cells)
        {
            if (cells.size() < 3) {
                return std::nullopt;
            }
            const auto plugin = Trim(cells[0]);
            if (plugin.empty() || plugin.front() == '#') {
                return std::nullopt;
            }
            const auto id = ParseHexId(cells[1]);
            const auto key = Trim(cells[2]);
            if (!id || key.empty()) {
                return std::nullopt;
            }
            return Row{std::string{plugin}, *id, std::string{key}};
        }

        void SortByPath(std::vector<CsvFile>& files)
        {
            std::ranges::sort(files, {}, &CsvFile::path);
        }

        [[nodiscard]] std::string Folded(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            for (const char c : text) {
                out.push_back(AsciiLower(c));
            }
            return out;
        }

        // A content path as SkyrimNet keys one: trimmed, '\' as '/'.
        [[nodiscard]] std::string NormalPath(std::string_view path)
        {
            std::string out{Trim(path)};
            std::ranges::replace(out, '\\', '/');
            return out;
        }

        // --- Semantic versions ---------------------------------------------

        struct Version
        {
            std::uint64_t major = 0;
            std::uint64_t minor = 0;
            std::uint64_t patch = 0;
            std::vector<std::string> prerelease;
        };

        [[nodiscard]] bool AllDigits(std::string_view text) noexcept
        {
            return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
        }

        [[nodiscard]] std::uint64_t Number(std::string_view text) noexcept
        {
            std::uint64_t value = 0;
            for (const char c : text) {
                value = value * 10 + static_cast<std::uint64_t>(c - '0');
            }
            return value;
        }

        [[nodiscard]] std::vector<std::string_view> Split(std::string_view text, char separator)
        {
            std::vector<std::string_view> parts;
            std::size_t start = 0;
            while (true) {
                const auto end = text.find(separator, start);
                parts.push_back(text.substr(start, end - start));
                if (end == std::string_view::npos) {
                    return parts;
                }
                start = end + 1;
            }
        }

        // MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD], an optional leading 'v'.
        // Anything else is 0.0.0, as a manifest with no version is.
        [[nodiscard]] Version ParseVersion(std::string_view text)
        {
            text = Trim(text);
            if (text.starts_with('v') || text.starts_with('V')) {
                text.remove_prefix(1);
            }
            if (const auto build = text.find('+'); build != std::string_view::npos) {
                text = text.substr(0, build);
            }
            std::string_view prerelease;
            if (const auto dash = text.find('-'); dash != std::string_view::npos) {
                prerelease = text.substr(dash + 1);
                text = text.substr(0, dash);
            }
            const auto core = Split(text, '.');
            if (core.size() != 3 || !std::ranges::all_of(core, AllDigits)) {
                return {};
            }
            Version version{Number(core[0]), Number(core[1]), Number(core[2]), {}};
            if (!prerelease.empty()) {
                for (const auto part : Split(prerelease, '.')) {
                    version.prerelease.emplace_back(part);
                }
            }
            return version;
        }

        [[nodiscard]] int Sign(auto a, auto b) noexcept
        {
            return a < b ? -1 : (b < a ? 1 : 0);
        }

        // --- The stack ---------------------------------------------------------

        // The id a layer answers to in the registry and in pins.
        [[nodiscard]] std::string_view LayerId(const Layer& layer) noexcept
        {
            return layer.kind == LayerKind::Base ? kBaseId : std::string_view{layer.folder};
        }

        // Discovery's checks on a store folder: a library folder must carry
        // its own id in its manifest, and a base folder the base id.
        [[nodiscard]] bool Discoverable(const Layer& layer)
        {
            switch (layer.kind) {
            case LayerKind::Base:
                return (Folded(layer.folder) == kBaseId || Folded(layer.folder) == kBaseUpdateFolder)
                       && Folded(layer.manifestId) == kBaseId;
            case LayerKind::Library:
                return Folded(layer.manifestId) == Folded(layer.folder) && Folded(layer.folder) != kBaseId
                       && Folded(layer.folder) != kBaseUpdateFolder;
            default:
                return true;
            }
        }

        // Where a copy ranks on a version tie: library, then external, then
        // anything else.
        [[nodiscard]] int TieRank(LayerKind kind) noexcept
        {
            switch (kind) {
            case LayerKind::Library:
                return 0;
            case LayerKind::External:
                return 1;
            default:
                return 2;
            }
        }

        // Does `candidate` replace `held` as the copy of their shared id?
        [[nodiscard]] bool Supersedes(const Layer& candidate, const Layer& held)
        {
            if (const auto order = CompareVersions(candidate.version, held.version); order != 0) {
                return order > 0;
            }
            if (TieRank(candidate.kind) != TieRank(held.kind)) {
                return TieRank(candidate.kind) < TieRank(held.kind);
            }
            const auto candidateIsUpdate = Folded(candidate.folder) == kBaseUpdateFolder;
            const auto heldIsUpdate = Folded(held.folder) == kBaseUpdateFolder;
            return heldIsUpdate && !candidateIsUpdate;
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
        const auto name = TextUtf8::RepairWindows1252(raw);

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

    std::optional<std::uint32_t> ParseHexId(std::string_view text) noexcept
    {
        text = Trim(text);
        if (text.starts_with("0x") || text.starts_with("0X")) {
            text.remove_prefix(2);
        }
        if (text.empty() || text.size() > 8 || !std::ranges::all_of(text, IsHexDigit)) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (const char c : text) {
            const auto digit = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
            value = value * 16 + static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    Tables BuildTables(std::vector<CsvFile> uniqueFiles,
                       std::vector<CsvFile> genericFiles,
                       const IsLightPlugin& isLight,
                       const FormName& nameOf,
                       TableCensus* census)
    {
        Tables tables;
        TableCensus counts;
        SortByPath(uniqueFiles);
        SortByPath(genericFiles);

        for (const auto& file : uniqueFiles) {
            ForEachRow(file.text, [&](const std::vector<std::string>& cells) {
                const auto row = ReadRow(cells);
                if (!row) {
                    ++counts.skipped;
                    return;
                }
                // The mask comes from how the plugin is LOADED, not from the
                // id: a light plugin's rows carry twelve-bit ids.
                const auto localId = row->id & (isLight(row->plugin) ? 0xFFFu : 0xFFFFFFu);
                tables.unique.insert_or_assign(UniqueKey(row->plugin, localId), row->key);
                ++counts.uniqueRows;
            });
        }

        for (const auto& file : genericFiles) {
            ForEachRow(file.text, [&](const std::vector<std::string>& cells) {
                const auto row = ReadRow(cells);
                if (!row) {
                    ++counts.skipped;
                    return;
                }
                // Keyed by the form's NAME, as the game shows it, so that
                // every actor carrying that name finds the row.
                const auto name = TextUtf8::RepairWindows1252(nameOf(row->plugin, row->id));
                const auto trimmed = Trim(name);
                if (trimmed.empty()) {
                    ++counts.genericUnnamed;
                    return;
                }
                tables.generic.insert_or_assign(Slug(trimmed), row->key);
                ++counts.genericRows;
            });
        }

        if (census != nullptr) {
            *census = counts;
        }
        return tables;
    }

    Registry ParseRegistry(std::string_view json)
    {
        Registry registry;
        const auto document = nlohmann::json::parse(json, nullptr, false);
        if (!document.is_object()) {
            return registry;
        }
        // A file written by a newer SkyrimNet is quarantined, not guessed at.
        if (const auto schema = document.find("schema_version");
            schema != document.end() && schema->is_number_integer() && schema->get<std::int64_t>() > 1) {
            return registry;
        }

        // Records. nlohmann iterates keys in byte order, which is also the
        // order SkyrimNet meets them in, so the first of a case-folded pair is
        // the one kept.
        std::map<std::string, std::string> byFolded;
        if (const auto plugins = document.find("plugins"); plugins != document.end() && plugins->is_object()) {
            for (const auto& [rawId, record] : plugins->items()) {
                const std::string id{Trim(rawId)};
                const auto folded = Folded(id);
                if (id.empty() || folded == kBaseId || !record.is_object() || byFolded.contains(folded)) {
                    continue;
                }
                Registry::Plugin plugin;
                if (const auto enabled = record.find("enabled"); enabled != record.end() && enabled->is_boolean()) {
                    plugin.enabled = enabled->get<bool>();
                }
                if (const auto missing = record.find("missing"); missing != record.end() && missing->is_boolean()) {
                    plugin.missing = missing->get<bool>();
                }
                byFolded.emplace(folded, id);
                registry.plugins.emplace(id, plugin);
            }
        }

        // Priority: entries naming no record, and repeats, are dropped; a
        // record with no entry is appended at the bottom.
        std::unordered_set<std::string> ranked;
        if (const auto priority = document.find("priority"); priority != document.end() && priority->is_array()) {
            for (const auto& entry : *priority) {
                if (!entry.is_string()) {
                    continue;
                }
                const auto record = byFolded.find(Folded(Trim(entry.get<std::string>())));
                if (record != byFolded.end() && ranked.insert(record->first).second) {
                    registry.priority.push_back(record->second);
                }
            }
        }
        for (const auto& [folded, id] : byFolded) {
            if (ranked.insert(folded).second) {
                registry.priority.push_back(id);
            }
        }

        // Pins: a path to the plugin id chosen for it.
        std::unordered_set<std::string> pinned;
        if (const auto pins = document.find("pins"); pins != document.end() && pins->is_object()) {
            for (const auto& [rawPath, id] : pins->items()) {
                auto path = NormalPath(rawPath);
                if (path.empty() || !id.is_string() || Trim(id.get<std::string>()).empty()
                    || !pinned.insert(Folded(path)).second) {
                    continue;
                }
                registry.pins.emplace(std::move(path), std::string{Trim(id.get<std::string>())});
            }
        }
        return registry;
    }

    int CompareVersions(std::string_view a, std::string_view b)
    {
        const auto left = ParseVersion(a);
        const auto right = ParseVersion(b);
        for (const auto order :
             {Sign(left.major, right.major), Sign(left.minor, right.minor), Sign(left.patch, right.patch)}) {
            if (order != 0) {
                return order;
            }
        }
        // A release outranks any pre-release of it.
        if (left.prerelease.empty() != right.prerelease.empty()) {
            return left.prerelease.empty() ? 1 : -1;
        }
        for (std::size_t i = 0; i < left.prerelease.size() && i < right.prerelease.size(); ++i) {
            const auto& x = left.prerelease[i];
            const auto& y = right.prerelease[i];
            const bool xNumeric = AllDigits(x);
            const bool yNumeric = AllDigits(y);
            int order = 0;
            if (xNumeric && yNumeric) {
                order = Sign(Number(x), Number(y));
            } else if (xNumeric != yNumeric) {
                order = xNumeric ? -1 : 1;
            } else {
                order = Sign(x, y);
            }
            if (order != 0) {
                return order;
            }
        }
        return Sign(left.prerelease.size(), right.prerelease.size());
    }

    Stack BuildStack(const std::vector<Layer>& layers, const Registry& registry)
    {
        Stack stack;
        stack.pins = registry.pins;

        // One copy per id.
        std::map<std::string, const Layer*> copies;
        const Layer* save = nullptr;
        const Layer* overlay = nullptr;
        for (const auto& layer : layers) {
            if (layer.kind == LayerKind::Save) {
                save = save == nullptr ? &layer : save;
                continue;
            }
            if (layer.kind == LayerKind::Overlay) {
                overlay = overlay == nullptr ? &layer : overlay;
                continue;
            }
            if (!Discoverable(layer)) {
                continue;
            }
            auto [held, inserted] = copies.try_emplace(Folded(LayerId(layer)), &layer);
            if (!inserted && Supersedes(layer, *held->second)) {
                held->second = &layer;
            }
        }

        if (save != nullptr) {
            stack.layers.push_back(save);
        }
        if (overlay != nullptr) {
            stack.layers.push_back(overlay);
        }
        // Plugins, in priority order; a layer the registry does not list,
        // or lists as disabled or missing, takes no part.
        for (const auto& id : registry.priority) {
            const auto copy = copies.find(Folded(id));
            if (copy == copies.end() || copy->second->kind == LayerKind::Base) {
                continue;
            }
            const auto& record = registry.plugins.at(id);
            if (record.enabled && !record.missing) {
                stack.layers.push_back(copy->second);
            }
        }
        if (const auto base = copies.find(std::string{kBaseId}); base != copies.end()) {
            stack.layers.push_back(base->second);
        }
        return stack;
    }

    const Layer* Winner(const Stack& stack, std::string_view path)
    {
        const auto key = NormalPath(path);
        const Layer* first = nullptr;
        for (const auto* layer : stack.layers) {
            if (layer->paths.contains(key)) {
                first = layer;
                break;
            }
        }
        // Nobody has it; or the save layer does, which outranks even a pin.
        if (first == nullptr || first->kind == LayerKind::Save) {
            return first;
        }
        if (const auto pin = stack.pins.find(key); pin != stack.pins.end()) {
            for (const auto* layer : stack.layers) {
                if (layer->kind != LayerKind::Save && LayerId(*layer) == pin->second && layer->paths.contains(key)) {
                    return layer;
                }
            }
        }
        return first;
    }

    Resolution Resolve(const ActorFacts& facts, std::string_view bioName, const Tables& tables, const Stack& stack)
    {
        if (bioName.empty()) {
            return {};
        }
        const auto attempt = [&](Tier tier, std::string path) -> std::optional<Resolution> {
            if (const auto* layer = Winner(stack, path); layer != nullptr) {
                return Resolution{tier, std::move(path), layer};
            }
            return std::nullopt;
        };

        if (auto dynamic =
                attempt(Tier::Dynamic, std::format("prompts/characters/dynamic/{}.dynamic.prompt", bioName))) {
            return *dynamic;
        }
        if (auto own = attempt(Tier::Own, std::format("prompts/characters/{}.prompt", bioName))) {
            return *own;
        }

        if (!facts.baseUnique) {
            // Entity::GetDisplayName: the display name, else the base's.
            const auto name =
                TextUtf8::RepairWindows1252(!facts.displayName.empty() ? facts.displayName : facts.baseName);
            // A generic-table hit is used as it stands; otherwise the slug.
            const auto genericKey = [&](std::string_view text, bool trimFirst) {
                if (const auto trimmed = Trim(text); !trimmed.empty()) {
                    if (const auto hit = tables.generic.find(Slug(trimmed)); hit != tables.generic.end()) {
                        return hit->second;
                    }
                }
                return Slug(trimFirst ? Trim(text) : text);
            };

            // First the bracketed tag: "Boren Vallkirson [Courier]" is a courier.
            const auto open = name.find('[');
            const auto close = name.find(']');
            if (open != std::string::npos && close != std::string::npos && open < close) {
                if (const auto tag = Trim(std::string_view{name}.substr(open + 1, close - open - 1)); !tag.empty()) {
                    if (auto generic =
                            attempt(Tier::Generic,
                                    std::format("prompts/characters/{}_generic.prompt", genericKey(tag, true)))) {
                        return *generic;
                    }
                }
            }
            // Then the whole name, untrimmed when it is slugged.
            if (auto generic = attempt(Tier::Generic,
                                       std::format("prompts/characters/{}_generic.prompt", genericKey(name, false)))) {
                return *generic;
            }
        }

        if (auto fallback = attempt(Tier::Default, "prompts/characters/default.prompt")) {
            return *fallback;
        }
        return {};
    }

} // namespace NarrativeEngine::CharacterBioResolver
