#include <FineRoads.h>

#include <BmpWriter.h>
#include <logger.h>
#include <MainThread.h>
#include <Settings.h>

#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NarrativeEngine::FineRoads
{
    namespace
    {
        constexpr std::uint16_t kNoTriangle = 0xFFFF;
        constexpr float kCellUnits = 4096.0f;

        // A flagged triangle, kept in raw form so the active graph can be
        // rebuilt from cached cells without re-reading engine memory.
        struct RawNode
        {
            RE::FormID mesh = 0;
            std::uint16_t triangle = kNoTriangle;
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            // Per edge: a same-mesh neighbour triangle, or a cross-mesh
            // portal target. An edge has at most one of the two.
            std::array<std::uint16_t, 3> neighborTri{kNoTriangle, kNoTriangle, kNoTriangle};
            std::array<RE::FormID, 3> portalMesh{0, 0, 0};
            std::array<std::uint16_t, 3> portalTri{kNoTriangle, kNoTriangle, kNoTriangle};
        };

        struct CellRoads
        {
            std::vector<RawNode> nodes;
            // Every navmesh this cell contributed, flagged triangles or
            // not. Needed to tell "portal into a mesh we have" from
            // "portal into a mesh we have not loaded" — the frontier
            // test — which a list of only road-bearing meshes can't do.
            std::vector<RE::FormID> meshes;
        };

        std::mutex g_mutex;
        std::unordered_map<RE::FormID, CellRoads> g_cache; // cell FormID -> extracted, session-scoped
        std::unordered_set<RE::FormID> g_activeCells;
        RE::FormID g_activeWorldSpace = 0;
        bool g_graphDirty = true;
        Graph g_graph;
        bool g_initialized = false;
        bool g_sinksRegistered = false;
        double g_secondsSinceBackstop = 0.0;

        // Set by the event sinks, consumed by Poll. Starts true so the
        // first poll after startup scans without waiting for an event.
        std::atomic<bool> g_rescanPending{true};
        // Bitmap dumps are gated on the active set actually changing, so
        // standing still doesn't rewrite the same image every interval.
        std::size_t g_lastDumpedSignature = 0;

        std::uint64_t TriangleKey(RE::FormID mesh, std::uint16_t triangle)
        {
            return (static_cast<std::uint64_t>(mesh) << 16) | static_cast<std::uint64_t>(triangle);
        }

        // ---------------- event sinks -------------------------------
        //
        // There is no cell-UNLOAD event: TESCellFullyLoadedEvent covers
        // loads only, and TESCellAttachDetachEvent is per-reference
        // rather than per-cell (high volume, wrong granularity). So the
        // events cannot themselves describe the active set — they only
        // say "something probably changed", and the grid scan in
        // SampleGrid remains the source of truth.
        //
        // In practice that gap barely matters: the grid shifts as a
        // unit, so cells unloading are almost always accompanied by
        // cells loading, and the resulting rescan sees both. The
        // backstop interval covers the remainder.

        struct CellLoadedSink : public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const RE::TESCellFullyLoadedEvent*,
                                                  RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
            {
                g_rescanPending.store(true, std::memory_order_release);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        struct LoadGameSink : public RE::BSTEventSink<RE::TESLoadGameEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const RE::TESLoadGameEvent*,
                                                  RE::BSTEventSource<RE::TESLoadGameEvent>*) override
            {
                g_rescanPending.store(true, std::memory_order_release);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        struct FastTravelEndSink : public RE::BSTEventSink<RE::TESFastTravelEndEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const RE::TESFastTravelEndEvent*,
                                                  RE::BSTEventSource<RE::TESFastTravelEndEvent>*) override
            {
                g_rescanPending.store(true, std::memory_order_release);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        CellLoadedSink* GetCellLoadedSink()
        {
            static CellLoadedSink sink;
            return &sink;
        }

        LoadGameSink* GetLoadGameSink()
        {
            static LoadGameSink sink;
            return &sink;
        }

        FastTravelEndSink* GetFastTravelEndSink()
        {
            static FastTravelEndSink sink;
            return &sink;
        }

        // ---------------- extraction (main thread) ------------------

        // Pull the road triangles out of one loaded cell.
        //
        // Edge encoding: `triangles[e]` means one of two different things
        // depending on the matching kEdgeN_Link flag. With the flag set
        // it indexes `extraEdgeInfo` (a portal to another navmesh);
        // without it, it's a triangle index within this same mesh, and
        // 0xFFFF means no neighbour at all.
        //
        // That reading is inferred from the record format rather than
        // documented in CommonLibSSE-NG, but it is corroborated by the
        // Spriggit export: triangles carrying an `EdgeLink_N_M` flag hold
        // small values in that slot consistent with an index into a short
        // portal array, while unflagged slots hold triangle-range values
        // or -1. Every index below is bounds-checked regardless, so a
        // misreading costs us adjacency rather than stability.
        CellRoads ExtractCell(RE::TESObjectCELL* cell)
        {
            CellRoads out;
            if (!cell) {
                return out;
            }
            auto* navMeshes = cell->GetRuntimeData().navMeshes;
            if (!navMeshes) {
                return out; // some ocean / border cells genuinely have none
            }

            using TriangleFlag = RE::BSNavmeshTriangle::TriangleFlag;
            constexpr std::array<TriangleFlag, 3> kEdgeLinkFlags = {
                TriangleFlag::kEdge0_Link,
                TriangleFlag::kEdge1_Link,
                TriangleFlag::kEdge2_Link,
            };

            for (const auto& meshPtr : navMeshes->navMeshes) {
                auto* mesh = meshPtr.get();
                if (!mesh) {
                    continue;
                }
                const auto meshID = mesh->GetFormID();
                out.meshes.push_back(meshID);

                const auto& verts = mesh->vertices;
                const auto& tris = mesh->triangles;
                for (std::size_t t = 0; t < tris.size(); ++t) {
                    const auto& tri = tris[t];
                    if (!tri.triangleFlags.any(TriangleFlag::kPreferred)) {
                        continue;
                    }
                    if (tri.triangleFlags.any(TriangleFlag::kDeleted)) {
                        continue;
                    }
                    const auto i0 = tri.vertices[0];
                    const auto i1 = tri.vertices[1];
                    const auto i2 = tri.vertices[2];
                    if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) {
                        continue;
                    }

                    RawNode node;
                    node.mesh = meshID;
                    node.triangle = static_cast<std::uint16_t>(t);
                    const auto& a = verts[i0].location;
                    const auto& b = verts[i1].location;
                    const auto& c = verts[i2].location;
                    node.x = (a.x + b.x + c.x) / 3.0f;
                    node.y = (a.y + b.y + c.y) / 3.0f;
                    node.z = (a.z + b.z + c.z) / 3.0f;

                    for (int e = 0; e < 3; ++e) {
                        const std::uint16_t raw = tri.triangles[e];
                        if (tri.triangleFlags.any(kEdgeLinkFlags[e])) {
                            if (raw < mesh->extraEdgeInfo.size()) {
                                const auto& info = mesh->extraEdgeInfo[raw];
                                if (info.type == RE::EDGE_EXTRA_INFO_TYPE::kPortal) {
                                    node.portalMesh[e] = info.portal.otherMeshID;
                                    node.portalTri[e] = info.portal.triangle;
                                }
                            }
                        } else if (raw != kNoTriangle && raw < tris.size()) {
                            node.neighborTri[e] = raw;
                        }
                    }
                    out.nodes.push_back(node);
                }
            }
            return out;
        }

        struct GridSample
        {
            bool valid = false;
            RE::FormID worldSpace = 0;
            std::unordered_set<RE::FormID> loadedCells;
            std::unordered_map<RE::FormID, CellRoads> extracted;
        };

        // Runs on the main thread. `alreadyCached` is passed in so this
        // never touches g_mutex — the plugin thread must not hold it
        // across a blocking MainThread::Run.
        GridSample SampleGrid(const std::unordered_set<RE::FormID>& alreadyCached)
        {
            GridSample sample;
            auto* tes = RE::TES::GetSingleton();
            if (!tes) {
                return sample;
            }
            if (tes->interiorCell) {
                // Indoors: no exterior grid. Valid result, empty set —
                // the caller clears the active graph rather than keeping
                // stale nodes from wherever the player last was outside.
                sample.valid = true;
                return sample;
            }
            auto* grid = tes->gridCells;
            if (!grid || !grid->cells) {
                return sample;
            }

            sample.valid = true;
            for (std::uint32_t gx = 0; gx < grid->length; ++gx) {
                for (std::uint32_t gy = 0; gy < grid->length; ++gy) {
                    auto* cell = grid->GetCell(gx, gy);
                    if (!cell || cell->IsInteriorCell()) {
                        continue;
                    }
                    if (!cell->IsAttached()) {
                        continue; // in the grid but not loaded yet
                    }
                    const auto cellID = cell->GetFormID();
                    sample.loadedCells.insert(cellID);
                    if (sample.worldSpace == 0) {
                        if (auto* ws = cell->GetRuntimeData().worldSpace) {
                            sample.worldSpace = ws->GetFormID();
                        }
                    }
                    if (!alreadyCached.contains(cellID)) {
                        sample.extracted.emplace(cellID, ExtractCell(cell));
                    }
                }
            }
            return sample;
        }

        // ---------------- active graph (plugin thread) --------------

        void RebuildGraphLocked()
        {
            g_graph = Graph{};
            g_graph.worldSpace = g_activeWorldSpace;

            // Every navmesh present in the active region, road-bearing or
            // not. A portal leaving this set is what makes a frontier.
            std::unordered_set<RE::FormID> activeMeshes;
            for (const auto cellID : g_activeCells) {
                const auto it = g_cache.find(cellID);
                if (it == g_cache.end()) {
                    continue;
                }
                for (const auto meshID : it->second.meshes) {
                    activeMeshes.insert(meshID);
                }
            }

            std::vector<const RawNode*> raws;
            std::unordered_map<std::uint64_t, std::size_t> byTriangle;
            for (const auto cellID : g_activeCells) {
                const auto it = g_cache.find(cellID);
                if (it == g_cache.end()) {
                    continue;
                }
                for (const auto& raw : it->second.nodes) {
                    byTriangle.emplace(TriangleKey(raw.mesh, raw.triangle), raws.size());
                    raws.push_back(&raw);
                }
            }

            g_graph.nodes.reserve(raws.size());
            g_graph.adjacency.assign(raws.size(), {});
            for (const auto* raw : raws) {
                g_graph.nodes.push_back(Node{raw->x, raw->y, raw->z, false});
            }

            auto link = [](std::size_t a, std::size_t b) {
                if (a == b) {
                    return;
                }
                auto& na = g_graph.adjacency[a];
                if (std::find(na.begin(), na.end(), b) == na.end()) {
                    na.push_back(b);
                }
            };

            std::size_t frontierCount = 0;
            for (std::size_t i = 0; i < raws.size(); ++i) {
                const auto* raw = raws[i];
                for (int e = 0; e < 3; ++e) {
                    if (raw->neighborTri[e] != kNoTriangle) {
                        const auto it = byTriangle.find(TriangleKey(raw->mesh, raw->neighborTri[e]));
                        if (it != byTriangle.end()) {
                            link(i, it->second);
                            link(it->second, i);
                        }
                        // Neighbour exists but isn't flagged: that's the
                        // side of the road ribbon, not a boundary.
                        continue;
                    }
                    if (raw->portalMesh[e] == 0) {
                        continue;
                    }
                    if (!activeMeshes.contains(raw->portalMesh[e])) {
                        if (!g_graph.nodes[i].frontier) {
                            g_graph.nodes[i].frontier = true;
                            ++frontierCount;
                        }
                        continue;
                    }
                    const auto it = byTriangle.find(TriangleKey(raw->portalMesh[e], raw->portalTri[e]));
                    if (it != byTriangle.end()) {
                        link(i, it->second);
                        link(it->second, i);
                    }
                }
            }

            std::size_t edgeCount = 0;
            for (const auto& adj : g_graph.adjacency) {
                edgeCount += adj.size();
            }
            edgeCount /= 2;

            logger::debug("FineRoads: active graph rebuilt — {} cell(s), {} node(s), {} edge(s), {} frontier",
                          g_activeCells.size(),
                          g_graph.nodes.size(),
                          edgeCount,
                          frontierCount);
            g_graphDirty = false;
        }

        // ---------------- debug bitmap ------------------------------

        constexpr Bmp::RGB kWhite{255, 255, 255};
        constexpr Bmp::RGB kEdge{150, 150, 150};
        constexpr Bmp::RGB kNodeColor{200, 40, 40};
        constexpr Bmp::RGB kFrontierColor{20, 60, 200};

        void WriteDebugBitmapLocked()
        {
            if (g_graph.nodes.empty()) {
                return;
            }
            float minX = std::numeric_limits<float>::max();
            float maxX = std::numeric_limits<float>::lowest();
            float minY = std::numeric_limits<float>::max();
            float maxY = std::numeric_limits<float>::lowest();
            for (const auto& n : g_graph.nodes) {
                minX = std::min(minX, n.x);
                maxX = std::max(maxX, n.x);
                minY = std::min(minY, n.y);
                maxY = std::max(maxY, n.y);
            }
            minX -= 512.0f;
            maxX += 512.0f;
            minY -= 512.0f;
            maxY += 512.0f;

            const float unitsPerPixel =
                std::max(1.0f, static_cast<float>(Settings::Get().fineRoadsBitmapUnitsPerPixel));
            const int width = std::max(1, static_cast<int>((maxX - minX) / unitsPerPixel) + 1);
            const int height = std::max(1, static_cast<int>((maxY - minY) / unitsPerPixel) + 1);
            if (width > 8192 || height > 8192) {
                return;
            }

            std::vector<Bmp::RGB> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), kWhite);
            auto set = [&](int x, int y, Bmp::RGB c) {
                if (x < 0 || y < 0 || x >= width || y >= height) {
                    return;
                }
                pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = c;
            };
            auto toPixel = [&](const Node& n) {
                return std::pair<int, int>{static_cast<int>((n.x - minX) / unitsPerPixel),
                                           static_cast<int>((maxY - n.y) / unitsPerPixel)};
            };
            auto line = [&](int x0, int y0, int x1, int y1, Bmp::RGB c) {
                const int dx = std::abs(x1 - x0);
                const int dy = -std::abs(y1 - y0);
                const int sx = x0 < x1 ? 1 : -1;
                const int sy = y0 < y1 ? 1 : -1;
                int err = dx + dy;
                while (true) {
                    set(x0, y0, c);
                    if (x0 == x1 && y0 == y1) {
                        break;
                    }
                    const int e2 = 2 * err;
                    if (e2 >= dy) {
                        err += dy;
                        x0 += sx;
                    }
                    if (e2 <= dx) {
                        err += dx;
                        y0 += sy;
                    }
                }
            };

            for (std::size_t i = 0; i < g_graph.nodes.size(); ++i) {
                const auto [ax, ay] = toPixel(g_graph.nodes[i]);
                for (const auto j : g_graph.adjacency[i]) {
                    if (j < i) {
                        continue;
                    }
                    const auto [bx, by] = toPixel(g_graph.nodes[j]);
                    line(ax, ay, bx, by, kEdge);
                }
            }
            for (const auto& n : g_graph.nodes) {
                const auto [px, py] = toPixel(n);
                const auto c = n.frontier ? kFrontierColor : kNodeColor;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        set(px + dx, py + dy, c);
                    }
                }
            }

            auto logsFolder = SKSE::log::log_directory();
            if (!logsFolder) {
                return;
            }
            const auto path = *logsFolder / "NarrativeEngine_FineRoads.bmp";
            if (Bmp::Write24(path, width, height, [&](int x, int y) {
                    return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                                  + static_cast<std::size_t>(x)];
                })) {
                logger::info("FineRoads: wrote bitmap '{}' ({}x{}, {} node(s), {:.0f} units/pixel)",
                             path.string(),
                             width,
                             height,
                             g_graph.nodes.size(),
                             unitsPerPixel);
            }
        }
    } // namespace

    void Initialize()
    {
        {
            std::scoped_lock lock(g_mutex);
            if (g_initialized) {
                return;
            }
            g_initialized = true;
        }

        // Event-driven invalidation. The sinks do not read the grid
        // themselves — they only raise a flag that the next Tick poll
        // consumes. That keeps engine-thread work to a single atomic
        // store, and it coalesces naturally: crossing one cell boundary
        // fires TESCellFullyLoadedEvent once per newly loaded cell, and
        // all of those collapse into one rescan.
        if (auto* src = RE::ScriptEventSourceHolder::GetSingleton()) {
            src->AddEventSink<RE::TESCellFullyLoadedEvent>(GetCellLoadedSink());
            src->AddEventSink<RE::TESLoadGameEvent>(GetLoadGameSink());
            src->AddEventSink<RE::TESFastTravelEndEvent>(GetFastTravelEndSink());
            g_sinksRegistered = true;
        } else {
            logger::error("FineRoads: ScriptEventSourceHolder unavailable; sinks not registered "
                          "(the backstop rescan still covers grid changes)");
        }

        logger::info(
            "FineRoads: initialized (enabled={}, sinks={})", Settings::Get().fineRoadsEnabled, g_sinksRegistered);
    }

    void Poll(const PluginThread::Token& pt, double unpausedElapsedSeconds)
    {
        if (!Settings::Get().fineRoadsEnabled) {
            return;
        }

        // Normal path: a sink flagged a change and we rescan on the very
        // next tick. The backstop only fires when no event has arrived
        // for a while — it exists because cell unloads have no event of
        // their own, so an unload unaccompanied by any load would
        // otherwise leave stale cells in the active set indefinitely.
        g_secondsSinceBackstop += unpausedElapsedSeconds;
        const double backstop = static_cast<double>(std::max(1, Settings::Get().fineRoadsBackstopSeconds));
        const bool flagged = g_rescanPending.exchange(false, std::memory_order_acq_rel);
        if (!flagged && g_secondsSinceBackstop < backstop) {
            return;
        }
        g_secondsSinceBackstop = 0.0;

        // Snapshot which cells we've already extracted, without holding
        // the lock into the main-thread call below.
        std::unordered_set<RE::FormID> cached;
        {
            std::scoped_lock lock(g_mutex);
            cached.reserve(g_cache.size());
            for (const auto& [cellID, roads] : g_cache) {
                cached.insert(cellID);
            }
        }

        auto sample = MainThread::Run(pt, [&cached](const MainThread::Token&) { return SampleGrid(cached); });
        if (!sample.valid) {
            return;
        }

        {
            std::scoped_lock lock(g_mutex);
            for (auto& [cellID, roads] : sample.extracted) {
                g_cache.emplace(cellID, std::move(roads));
            }
            if (sample.loadedCells != g_activeCells || sample.worldSpace != g_activeWorldSpace) {
                g_activeCells = std::move(sample.loadedCells);
                g_activeWorldSpace = sample.worldSpace;
                g_graphDirty = true;
            }
            if (g_graphDirty) {
                RebuildGraphLocked();
                if (Settings::Get().fineRoadsDebugBitmap) {
                    // Signature keeps a stationary player from rewriting
                    // an identical image every interval.
                    const std::size_t signature = g_graph.nodes.size() * 31 + g_activeCells.size();
                    if (signature != g_lastDumpedSignature) {
                        g_lastDumpedSignature = signature;
                        WriteDebugBitmapLocked();
                    }
                }
            }
        }
    }

    Graph Snapshot()
    {
        std::scoped_lock lock(g_mutex);
        if (g_graphDirty) {
            RebuildGraphLocked();
        }
        return g_graph;
    }

    std::size_t NodeCount()
    {
        std::scoped_lock lock(g_mutex);
        return g_graph.nodes.size();
    }
} // namespace NarrativeEngine::FineRoads
