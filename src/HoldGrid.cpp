#include <HoldGrid.h>

#include <logger.h>
#include <Settings.h>

#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NarrativeEngine::HoldGrid
{
    namespace
    {
        // Cap on the parent-chain walk when seeding. Vanilla chains
        // are 2-4 deep; guard defensively against malformed data.
        constexpr int kMaxParentDepth = 16;

        // Pack (worldspaceFormID, X, Y) into a 64-bit key. FormIDs are
        // 32 bits; cell coords fit in int16. Layout:
        //   bits [63..32] = worldspace FormID
        //   bits [31..16] = Y (as uint16 for bit-preserving cast)
        //   bits [15..0]  = X (as uint16)
        std::uint64_t MakeKey(RE::FormID ws, std::int16_t x, std::int16_t y)
        {
            return (static_cast<std::uint64_t>(ws) << 32)
                   | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(y)) << 16)
                   | static_cast<std::uint64_t>(static_cast<std::uint16_t>(x));
        }

        std::mutex g_mutex;
        std::unordered_map<std::uint64_t, RE::FormID> g_grid;
        bool g_initialized = false;

        RE::BGSKeyword* ResolveLocTypeHold()
        {
            auto* form = RE::TESForm::LookupByEditorID("LocTypeHold");
            if (!form)
                return nullptr;
            return form->As<RE::BGSKeyword>();
        }

        // Walk up parentLoc from `loc` looking for the first ancestor
        // tagged with LocTypeHold. Returns the FormID of that ancestor,
        // or 0 if none found within kMaxParentDepth.
        RE::FormID FindHoldForLocation(RE::BGSLocation* loc, RE::BGSKeyword* holdKw)
        {
            for (int depth = 0; loc && depth < kMaxParentDepth; ++depth) {
                if (loc->HasKeyword(holdKw)) {
                    return loc->GetFormID();
                }
                loc = loc->parentLoc;
            }
            return 0;
        }

        // -------- Debug bitmap ---------------------------------------

        struct RGB
        {
            std::uint8_t r;
            std::uint8_t g;
            std::uint8_t b;
        };

        // Curated distinct palette assigned to holds in FormID-sorted
        // order (deterministic across runs). Beyond kPalette.size()
        // holds, we fall back to a hash-derived color.
        constexpr std::array<RGB, 12> kPalette = {
            RGB{255, 60, 60},   // red
            RGB{60, 200, 60},   // green
            RGB{60, 60, 255},   // blue
            RGB{240, 220, 40},  // yellow
            RGB{200, 60, 200},  // magenta
            RGB{40, 220, 220},  // cyan
            RGB{255, 140, 40},  // orange
            RGB{255, 140, 200}, // pink
            RGB{40, 140, 140},  // teal
            RGB{100, 120, 160}, // slate
            RGB{150, 150, 60},  // olive
            RGB{140, 90, 50},   // brown
        };

        RGB HashColor(RE::FormID id)
        {
            // Three different multiplier constants to spread channels.
            return RGB{
                static_cast<std::uint8_t>(((id * 2654435761u) >> 16) & 0xFF),
                static_cast<std::uint8_t>(((id * 4055993439u) >> 16) & 0xFF),
                static_cast<std::uint8_t>(((id * 3266489917u) >> 16) & 0xFF),
            };
        }

        // Minimal 24-bit uncompressed BMP writer. Rows are bottom-up
        // in the file, padded to 4-byte boundaries. Writes width x
        // height pixels sampled via `pixelAt(pixelX, pixelY)`.
        // pixelY=0 is the TOP of the image; the writer flips
        // internally for BMP's bottom-up convention.
        template <class PixelFn>
        bool WriteBmp24(const std::filesystem::path& path, int width, int height, PixelFn pixelAt)
        {
            if (width <= 0 || height <= 0) {
                return false;
            }
            const int rowSize = ((width * 3 + 3) / 4) * 4;
            const std::uint32_t pixelDataSize =
                static_cast<std::uint32_t>(rowSize) * static_cast<std::uint32_t>(height);
            const std::uint32_t fileSize = 14 + 40 + pixelDataSize;

            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                return false;
            }

            auto writeU16 = [&](std::uint16_t v) {
                std::uint8_t bytes[2] = {
                    static_cast<std::uint8_t>(v & 0xFF),
                    static_cast<std::uint8_t>((v >> 8) & 0xFF),
                };
                out.write(reinterpret_cast<const char*>(bytes), 2);
            };
            auto writeU32 = [&](std::uint32_t v) {
                std::uint8_t bytes[4] = {
                    static_cast<std::uint8_t>(v & 0xFF),
                    static_cast<std::uint8_t>((v >> 8) & 0xFF),
                    static_cast<std::uint8_t>((v >> 16) & 0xFF),
                    static_cast<std::uint8_t>((v >> 24) & 0xFF),
                };
                out.write(reinterpret_cast<const char*>(bytes), 4);
            };
            auto writeS32 = [&](std::int32_t v) { writeU32(static_cast<std::uint32_t>(v)); };

            // BITMAPFILEHEADER (14 bytes)
            writeU16(0x4D42); // 'BM'
            writeU32(fileSize);
            writeU16(0);
            writeU16(0);
            writeU32(54);
            // BITMAPINFOHEADER (40 bytes)
            writeU32(40);
            writeS32(width);
            writeS32(height); // positive = bottom-up rows
            writeU16(1);
            writeU16(24);
            writeU32(0); // BI_RGB
            writeU32(pixelDataSize);
            writeS32(2835); // 72 DPI in pixels/meter
            writeS32(2835);
            writeU32(0);
            writeU32(0);

            // Pixel rows: file row 0 = bottom = image y = (height-1).
            std::vector<std::uint8_t> row(static_cast<std::size_t>(rowSize), 0);
            for (int fileRow = 0; fileRow < height; ++fileRow) {
                const int imageY = height - 1 - fileRow;
                for (int x = 0; x < width; ++x) {
                    const RGB c = pixelAt(x, imageY);
                    // BMP pixel byte order is BGR.
                    row[static_cast<std::size_t>(x) * 3 + 0] = c.b;
                    row[static_cast<std::size_t>(x) * 3 + 1] = c.g;
                    row[static_cast<std::size_t>(x) * 3 + 2] = c.r;
                }
                out.write(reinterpret_cast<const char*>(row.data()), rowSize);
            }
            return out.good();
        }

        // Write one BMP for this worldspace showing the fill partition.
        // Image coordinate convention: image y=0 is the TOP, which
        // corresponds to Skyrim's maxY (north). So north is up.
        void WriteWorldspaceBitmap(RE::TESWorldSpace* ws,
                                   const std::unordered_map<std::uint64_t, RE::FormID>& grid,
                                   const std::unordered_set<std::uint64_t>& seedKeys,
                                   std::int16_t minX,
                                   std::int16_t maxX,
                                   std::int16_t minY,
                                   std::int16_t maxY)
        {
            const RE::FormID wsID = ws->GetFormID();

            // Collect distinct hold FormIDs for this worldspace, sorted
            // by FormID so palette assignment is stable across runs.
            std::vector<RE::FormID> holds;
            for (const auto& entry : grid) {
                if ((entry.first >> 32) != wsID) {
                    continue;
                }
                if (std::find(holds.begin(), holds.end(), entry.second) == holds.end()) {
                    holds.push_back(entry.second);
                }
            }
            std::sort(holds.begin(), holds.end());

            std::unordered_map<RE::FormID, RGB> colorByHold;
            for (std::size_t i = 0; i < holds.size(); ++i) {
                colorByHold[holds[i]] = (i < kPalette.size()) ? kPalette[i] : HashColor(holds[i]);
            }

            // Log the palette so the user can decode the image.
            const char* wsEdid = ws->GetFormEditorID();
            logger::info("HoldGrid: bitmap palette for worldspace '{}':", (wsEdid && *wsEdid) ? wsEdid : "?");
            for (const auto id : holds) {
                auto* form = RE::TESForm::LookupByID(id);
                auto* loc = form ? form->As<RE::BGSLocation>() : nullptr;
                const char* edid = loc ? loc->GetFormEditorID() : nullptr;
                const char* full = loc ? loc->GetFullName() : nullptr;
                const auto& c = colorByHold[id];
                logger::info("HoldGrid:   [0x{:08X}] EditorID='{}' FullName='{}' RGB=({},{},{})",
                             id,
                             (edid && *edid) ? edid : "?",
                             (full && *full) ? full : "?",
                             c.r,
                             c.g,
                             c.b);
            }

            const int width = static_cast<int>(maxX - minX + 1);
            const int height = static_cast<int>(maxY - minY + 1);

            constexpr RGB kWhite{255, 255, 255};
            constexpr RGB kBlack{0, 0, 0};

            auto pixelAt = [&](int px, int py) -> RGB {
                // Image (0,0) = top-left = (minX, maxY).
                const std::int16_t skX = static_cast<std::int16_t>(minX + px);
                const std::int16_t skY = static_cast<std::int16_t>(maxY - py);
                const auto key = MakeKey(wsID, skX, skY);
                if (seedKeys.contains(key)) {
                    return kBlack;
                }
                const auto it = grid.find(key);
                if (it == grid.end()) {
                    return kWhite;
                }
                const auto colorIt = colorByHold.find(it->second);
                return (colorIt != colorByHold.end()) ? colorIt->second : kWhite;
            };

            auto logsFolder = SKSE::log::log_directory();
            if (!logsFolder) {
                logger::warn("HoldGrid: cannot write bitmap — SKSE log_directory unavailable");
                return;
            }
            const std::string wsName = (wsEdid && *wsEdid) ? wsEdid : "unknown";
            const auto path = *logsFolder / ("NarrativeEngine_HoldGrid_" + wsName + ".bmp");

            if (WriteBmp24(path, width, height, pixelAt)) {
                logger::info("HoldGrid: wrote bitmap '{}' ({}x{})", path.string(), width, height);
            } else {
                logger::warn("HoldGrid: failed to write bitmap '{}'", path.string());
            }
        }
    } // namespace

    void Initialize()
    {
        std::scoped_lock lock(g_mutex);
        if (g_initialized) {
            return;
        }
        g_initialized = true;

        const auto startTime = std::chrono::steady_clock::now();

        auto* holdKw = ResolveLocTypeHold();
        if (!holdKw) {
            logger::warn("HoldGrid: LocTypeHold keyword did not resolve; grid disabled "
                         "(is powerofthree's Tweaks installed?)");
            return;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            logger::warn("HoldGrid: TESDataHandler unavailable; grid disabled");
            return;
        }

        std::size_t totalSeedCount = 0;
        std::size_t totalFilledCount = 0;
        std::size_t worldSpaceProcessed = 0;

        struct QueueEntry
        {
            std::int16_t x;
            std::int16_t y;
            RE::FormID hold;
        };

        // Iterate every worldspace. Each has its own coordinate system,
        // so BFS is per-worldspace.
        const auto& worldSpaces = dh->GetFormArray<RE::TESWorldSpace>();
        for (auto* ws : worldSpaces) {
            if (!ws) {
                continue;
            }

            const std::int16_t minX = ws->worldMapData.nwCellX;
            const std::int16_t maxX = ws->worldMapData.seCellX;
            // NW has higher Y than SE (positive Y = north in Skyrim).
            const std::int16_t minY = ws->worldMapData.seCellY;
            const std::int16_t maxY = ws->worldMapData.nwCellY;

            if (minX > maxX || minY > maxY) {
                continue; // degenerate or unset bounds
            }

            const RE::FormID wsID = ws->GetFormID();
            std::size_t wsFilledCount = 0;

            // Track seed keys per-worldspace so the debug bitmap can
            // draw them as black (distinguishing "was in the seed set"
            // from "was filled by BFS"). Local scope so it doesn't
            // outlive this worldspace's processing.
            std::unordered_set<std::uint64_t> seedKeys;

            // Seed phase: walk this worldspace's cellMap. Cells in
            // this map have their record data available; unloaded
            // cells still expose GetCoordinates + GetLocation.
            for (const auto& entry : ws->cellMap) {
                auto* cell = entry.second;
                if (!cell || cell->IsInteriorCell()) {
                    continue;
                }
                auto* loc = cell->GetLocation();
                if (!loc) {
                    continue;
                }
                const auto hold = FindHoldForLocation(loc, holdKw);
                if (!hold) {
                    continue;
                }
                const std::int16_t x = entry.first.x;
                const std::int16_t y = entry.first.y;
                const auto key = MakeKey(wsID, x, y);
                if (g_grid.emplace(key, hold).second) {
                    seedKeys.insert(key);
                }
            }

            if (seedKeys.empty()) {
                continue; // no seeds means nothing to fill
            }

            // Prune isolated small seed clusters. A cluster is a
            // connected component of same-hold seeds (4-neighbor
            // adjacency on the seed set); a cluster is "isolated" if
            // no OTHER same-hold seed exists within Manhattan
            // distance R of any cell in the cluster. Vanilla data
            // occasionally has small mis-tagged location groups that
            // would otherwise seed a bad fill front that propagates
            // to the map edge.
            const int maxClusterSize = Settings::Get().holdGridPruneMaxClusterSize;
            const int pruneRadius = Settings::Get().holdGridPruneIsolationRadius;
            std::size_t wsPrunedCount = 0;
            if (maxClusterSize > 0 && pruneRadius > 0) {
                constexpr std::array<int, 4> kNbrDX = {-1, 1, 0, 0};
                constexpr std::array<int, 4> kNbrDY = {0, 0, -1, 1};

                // Assign each seed cell a component id via flood-fill
                // on same-hold 4-neighbors. Components are indexed by
                // insertion order.
                std::unordered_map<std::uint64_t, int> cellToComponent;
                std::vector<std::vector<std::uint64_t>> componentCells;
                std::vector<RE::FormID> componentHolds;
                cellToComponent.reserve(seedKeys.size());

                for (const auto rootKey : seedKeys) {
                    if (cellToComponent.contains(rootKey)) {
                        continue;
                    }
                    const auto rootIt = g_grid.find(rootKey);
                    if (rootIt == g_grid.end()) {
                        continue;
                    }
                    const int compId = static_cast<int>(componentCells.size());
                    const RE::FormID compHold = rootIt->second;
                    componentCells.emplace_back();
                    componentHolds.push_back(compHold);

                    std::deque<std::uint64_t> fifo;
                    fifo.push_back(rootKey);
                    cellToComponent.emplace(rootKey, compId);
                    while (!fifo.empty()) {
                        const auto k = fifo.front();
                        fifo.pop_front();
                        componentCells[compId].push_back(k);
                        const std::int16_t x = static_cast<std::int16_t>(k & 0xFFFF);
                        const std::int16_t y = static_cast<std::int16_t>((k >> 16) & 0xFFFF);
                        for (int d = 0; d < 4; ++d) {
                            const auto nKey = MakeKey(wsID,
                                                      static_cast<std::int16_t>(x + kNbrDX[d]),
                                                      static_cast<std::int16_t>(y + kNbrDY[d]));
                            if (cellToComponent.contains(nKey)) {
                                continue;
                            }
                            const auto nIt = g_grid.find(nKey);
                            if (nIt == g_grid.end() || nIt->second != compHold) {
                                continue;
                            }
                            cellToComponent.emplace(nKey, compId);
                            fifo.push_back(nKey);
                        }
                    }
                }

                // For each small component, check isolation. Drop
                // components with no OUTSIDE same-hold seed within R.
                std::vector<std::uint64_t> toDrop;
                for (int compId = 0; compId < static_cast<int>(componentCells.size()); ++compId) {
                    const auto& cells = componentCells[compId];
                    if (static_cast<int>(cells.size()) > maxClusterSize) {
                        continue;
                    }
                    const RE::FormID compHold = componentHolds[compId];

                    bool foundNearby = false;
                    for (const auto k : cells) {
                        if (foundNearby)
                            break;
                        const std::int16_t x = static_cast<std::int16_t>(k & 0xFFFF);
                        const std::int16_t y = static_cast<std::int16_t>((k >> 16) & 0xFFFF);
                        for (int dy = -pruneRadius; dy <= pruneRadius && !foundNearby; ++dy) {
                            const int remain = pruneRadius - std::abs(dy);
                            for (int dx = -remain; dx <= remain && !foundNearby; ++dx) {
                                if (dx == 0 && dy == 0) {
                                    continue;
                                }
                                const auto testKey =
                                    MakeKey(wsID, static_cast<std::int16_t>(x + dx), static_cast<std::int16_t>(y + dy));
                                const auto testIt = g_grid.find(testKey);
                                if (testIt == g_grid.end() || testIt->second != compHold) {
                                    continue;
                                }
                                // Same-hold seed found — but skip if
                                // it's part of THIS component (that
                                // doesn't count as "outside").
                                const auto compIt = cellToComponent.find(testKey);
                                if (compIt != cellToComponent.end() && compIt->second == compId) {
                                    continue;
                                }
                                foundNearby = true;
                            }
                        }
                    }

                    if (!foundNearby) {
                        for (const auto k : cells) {
                            toDrop.push_back(k);
                        }
                    }
                }

                for (const auto key : toDrop) {
                    g_grid.erase(key);
                    seedKeys.erase(key);
                }
                wsPrunedCount = toDrop.size();
            }

            const std::size_t wsSeedCount = seedKeys.size();
            if (wsSeedCount == 0) {
                continue; // pruning cleared out everything
            }

            // Build the BFS queue from the (post-prune) seed set.
            std::deque<QueueEntry> queue;
            for (const auto key : seedKeys) {
                const auto hold = g_grid.at(key);
                const std::int16_t x = static_cast<std::int16_t>(key & 0xFFFF);
                const std::int16_t y = static_cast<std::int16_t>((key >> 16) & 0xFFFF);
                queue.push_back({x, y, hold});
            }

            // BFS fill: 4-neighbor adjacency, first-touch wins. Cells
            // outside the worldspace bounds are silently skipped.
            constexpr std::array<int, 4> kDX = {-1, 1, 0, 0};
            constexpr std::array<int, 4> kDY = {0, 0, -1, 1};

            while (!queue.empty()) {
                const auto front = queue.front();
                queue.pop_front();
                for (int d = 0; d < 4; ++d) {
                    const int nxi = static_cast<int>(front.x) + kDX[d];
                    const int nyi = static_cast<int>(front.y) + kDY[d];
                    if (nxi < minX || nxi > maxX || nyi < minY || nyi > maxY) {
                        continue;
                    }
                    const auto nx = static_cast<std::int16_t>(nxi);
                    const auto ny = static_cast<std::int16_t>(nyi);
                    const auto key = MakeKey(wsID, nx, ny);
                    if (g_grid.emplace(key, front.hold).second) {
                        queue.push_back({nx, ny, front.hold});
                        ++wsFilledCount;
                    }
                }
            }

            totalSeedCount += wsSeedCount;
            totalFilledCount += wsFilledCount;
            ++worldSpaceProcessed;

            const char* wsEdid = ws->GetFormEditorID();
            if (Settings::Get().traceMode) {
                logger::trace("HoldGrid: worldspace [0x{:08X}] '{}' — pruned {} isolated "
                              "singleton(s), seeded {} cells, filled {} more "
                              "(bounds X=[{},{}] Y=[{},{}])",
                              wsID,
                              (wsEdid && *wsEdid) ? wsEdid : "?",
                              wsPrunedCount,
                              wsSeedCount,
                              wsFilledCount,
                              minX,
                              maxX,
                              minY,
                              maxY);
            }

            // Debug bitmap dump — one BMP per worldspace, showing the
            // partition + seed cells. Gated on the master switch so
            // production builds skip the file I/O entirely.
            if (Settings::Get().holdGridDebugBitmap) {
                WriteWorldspaceBitmap(ws, g_grid, seedKeys, minX, maxX, minY, maxY);
            }
        }

        const auto elapsed = std::chrono::steady_clock::now() - startTime;
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        logger::info("HoldGrid: built ({} worldspace(s) contributed, {} seeded, {} filled, {} total entries; {}ms)",
                     worldSpaceProcessed,
                     totalSeedCount,
                     totalFilledCount,
                     g_grid.size(),
                     elapsedMs);
    }

    RE::FormID LookupCell(RE::TESWorldSpace* ws, std::int16_t x, std::int16_t y)
    {
        if (!ws) {
            return 0;
        }
        std::scoped_lock lock(g_mutex);
        const auto it = g_grid.find(MakeKey(ws->GetFormID(), x, y));
        return it != g_grid.end() ? it->second : 0;
    }

    RE::FormID LookupWorldPosition(RE::TESWorldSpace* ws, float x, float y)
    {
        constexpr float kCellUnits = 4096.0f;
        const auto cellX = static_cast<std::int16_t>(std::floor(x / kCellUnits));
        const auto cellY = static_cast<std::int16_t>(std::floor(y / kCellUnits));
        return LookupCell(ws, cellX, cellY);
    }

    RE::FormID LookupPlayer()
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            return 0;
        }
        auto* cell = pc->GetParentCell();
        if (!cell || cell->IsInteriorCell()) {
            return 0;
        }
        auto* ws = cell->GetRuntimeData().worldSpace;
        if (!ws) {
            return 0;
        }
        auto* coords = cell->GetCoordinates();
        if (!coords) {
            return 0;
        }
        return LookupCell(ws, static_cast<std::int16_t>(coords->cellX), static_cast<std::int16_t>(coords->cellY));
    }
} // namespace NarrativeEngine::HoldGrid
