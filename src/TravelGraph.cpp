#include <TravelGraph.h>

#include <BmpWriter.h>
#include <HoldGrid.h>
#include <logger.h>
#include <Settings.h>

#include <SKSE/SKSE.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NarrativeEngine::TravelGraph
{
    namespace
    {
        std::mutex g_mutex;
        bool g_initialized = false;

        std::vector<Node> g_nodes;
        // Parallel to g_nodes: neighbor indices, deduplicated.
        std::vector<std::vector<std::size_t>> g_adjacency;
        std::size_t g_edgeCount = 0;

        constexpr float kCellUnits = 4096.0f;

        // ---------------- form lookup -------------------------------

        struct FormScan
        {
            RE::NavMeshInfoMap* infoMap = nullptr;
            std::vector<RE::NavMesh*> navMeshes;
        };

        // Neither NAVI nor NAVM is registered in TESDataHandler's
        // per-type form arrays on Skyrim SE — both come back empty. The
        // global form table has them, so we scan that instead, doing
        // both types in one pass because the table holds every form in
        // the load order and walking it twice would be pure waste.
        FormScan ScanForms(RE::TESDataHandler* dh)
        {
            FormScan out;

            for (auto* form : dh->GetFormArray<RE::NavMeshInfoMap>()) {
                if (form) {
                    out.infoMap = form;
                    break;
                }
            }
            for (auto* form : dh->GetFormArray<RE::NavMesh>()) {
                if (form) {
                    out.navMeshes.push_back(form);
                }
            }

            const bool needNavi = out.infoMap == nullptr;
            const bool needMeshes = out.navMeshes.empty();
            if (!needNavi && !needMeshes) {
                return out;
            }

            const auto& [allForms, lock] = RE::TESForm::GetAllForms();
            const RE::BSReadLockGuard guard{lock};
            if (!allForms) {
                return out;
            }
            for (const auto& [id, form] : *allForms) {
                if (!form) {
                    continue;
                }
                const auto formType = form->GetFormType();
                if (needNavi && formType == RE::FormType::Navigation) {
                    if (!out.infoMap) {
                        out.infoMap = static_cast<RE::NavMeshInfoMap*>(form);
                    }
                } else if (needMeshes && formType == RE::FormType::NavMesh) {
                    out.navMeshes.push_back(static_cast<RE::NavMesh*>(form));
                }
            }
            return out;
        }

        // ---------------- BSNavmeshInfo layout ----------------------
        //
        // CommonLibSSE-NG forward-declares BSNavmeshInfo and defines no
        // members, but we need the position it holds: only ~15% of
        // navmeshes have a resident NavMesh form at kDataLoaded, while
        // NAVI knows about all of them. Without reading the info struct
        // most preferred-path hops can't be positioned and the graph
        // falls apart into disconnected islands.
        //
        // Rather than hardcode offsets — which would be a guess, and
        // would silently rot across game versions — we measure them at
        // startup. The minority of navmeshes that DO have a resident
        // form give us pairs of (BSNavmeshInfo*, known position, known
        // FormID); the offsets that reproduce those across thousands of
        // samples are found by search and validated before use.
        //
        // Measured on Skyrim SE 1.6.x: FormID at 0x00, NiPoint3 at 0x04.
        // We do not rely on those values — they are what calibration
        // finds, and if a future runtime moves them calibration finds
        // the new ones instead. Failure degrades to cell-centre
        // positions rather than bad data.

        struct CalibrationSample
        {
            const RE::BSNavmeshInfo* info;
            RE::FormID navMesh;
            float x;
            float y;
        };

        struct InfoLayout
        {
            bool valid = false;
            std::size_t pointOffset = 0;
            bool hasFormId = false;
            std::size_t formIdOffset = 0;
            double meanError = 0.0;
            std::size_t hits = 0;
            std::size_t sampled = 0;
        };

        // Window scanned for candidate offsets. Reads past the real
        // allocation are possible and are handled by SafeReadBytes.
        constexpr std::size_t kInfoWindow = 0x80;

        // Speculative offsets can land outside the allocation. SEH turns
        // that into a false return instead of a crash. Kept trivial
        // deliberately: a function using __try may not hold C++ objects
        // that require unwinding.
        bool SafeReadBytes(const void* src, void* dst, std::size_t n) noexcept
        {
            __try {
                std::memcpy(dst, src, n);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        InfoLayout CalibrateInfoLayout(const std::vector<CalibrationSample>& samples, bool verbose)
        {
            InfoLayout layout;
            if (samples.empty()) {
                return layout;
            }

            // Snapshot each sample's header once, so the SEH cost is per
            // sample rather than per (sample, offset).
            std::vector<std::array<std::uint8_t, kInfoWindow>> headers;
            std::vector<std::size_t> readable;
            headers.reserve(samples.size());
            for (std::size_t i = 0; i < samples.size(); ++i) {
                std::array<std::uint8_t, kInfoWindow> buf{};
                if (SafeReadBytes(samples[i].info, buf.data(), kInfoWindow)) {
                    headers.push_back(buf);
                    readable.push_back(i);
                }
            }
            if (readable.empty()) {
                logger::warn("TravelGraph: could not read any BSNavmeshInfo header; layout calibration failed");
                return layout;
            }
            layout.sampled = readable.size();

            // The point NAVI stores is a representative point for the
            // navmesh, not exactly our meshGrid midpoint, so the match
            // is approximate by nature. One cell is a generous bound
            // that still excludes unrelated float data comfortably.
            constexpr float kAcceptUnits = kCellUnits;

            struct Candidate
            {
                std::size_t offset;
                double meanError;
                std::size_t hits;
            };
            std::vector<Candidate> candidates;

            for (std::size_t offset = 0; offset + 12 <= kInfoWindow; offset += 4) {
                double total = 0.0;
                std::size_t hits = 0;
                for (std::size_t k = 0; k < readable.size(); ++k) {
                    float f[3];
                    std::memcpy(f, headers[k].data() + offset, sizeof(f));
                    if (!std::isfinite(f[0]) || !std::isfinite(f[1])) {
                        continue;
                    }
                    const auto& s = samples[readable[k]];
                    const double dx = static_cast<double>(f[0]) - s.x;
                    const double dy = static_cast<double>(f[1]) - s.y;
                    const double dist = std::sqrt(dx * dx + dy * dy);
                    if (dist <= kAcceptUnits) {
                        ++hits;
                        total += dist;
                    }
                }
                // A real hit matches nearly every sample. Requiring a
                // supermajority keeps coincidental float data out.
                if (hits * 10 >= readable.size() * 9) {
                    candidates.push_back({offset, total / static_cast<double>(hits), hits});
                }
            }

            if (candidates.empty()) {
                logger::warn("TravelGraph: no offset in the first 0x{:X} bytes of BSNavmeshInfo holds a position "
                             "matching {} calibration samples; falling back to cell-centre positions",
                             kInfoWindow,
                             readable.size());
                return layout;
            }

            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                return a.meanError < b.meanError;
            });
            layout.valid = true;
            layout.pointOffset = candidates.front().offset;
            layout.meanError = candidates.front().meanError;
            layout.hits = candidates.front().hits;

            // Independent cross-check: find the offset holding the
            // navmesh FormID. Not needed to position anything, but two
            // fields landing where expected is much stronger evidence
            // that we're reading the right struct than one field alone.
            for (std::size_t offset = 0; offset + 4 <= kInfoWindow; offset += 4) {
                std::size_t matches = 0;
                for (std::size_t k = 0; k < readable.size(); ++k) {
                    std::uint32_t id = 0;
                    std::memcpy(&id, headers[k].data() + offset, sizeof(id));
                    if (id == samples[readable[k]].navMesh) {
                        ++matches;
                    }
                }
                if (matches * 10 >= readable.size() * 9) {
                    layout.hasFormId = true;
                    layout.formIdOffset = offset;
                    break;
                }
            }

            logger::info("TravelGraph: BSNavmeshInfo calibrated — position at 0x{:02X} (mean error {:.1f} units "
                         "over {} of {} samples), FormID {}",
                         layout.pointOffset,
                         layout.meanError,
                         layout.hits,
                         readable.size(),
                         layout.hasFormId ? std::format("at 0x{:02X}", layout.formIdOffset)
                                          : std::string{"not located"});

            if (verbose) {
                for (std::size_t i = 1; i < candidates.size() && i < 4; ++i) {
                    logger::info("TravelGraph:   runner-up offset 0x{:02X} — mean error {:.1f} over {}",
                                 candidates[i].offset,
                                 candidates[i].meanError,
                                 candidates[i].hits);
                }
            }
            return layout;
        }

        // ---------------- info enumeration --------------------------

        struct InfoEntry
        {
            const RE::BSNavmeshInfo* info;
            RE::FormID worldSpace;
            std::int16_t cellX;
            std::int16_t cellY;
        };

        // Enumerate every navmesh the engine knows about, with the cell
        // it belongs to.
        //
        // ckNavMeshInfoMap is keyed by a packed uint64 whose layout was
        // read directly off live data (confirmed against negative cell
        // coordinates and a second worldspace):
        //   bits [63..32] = worldspace FormID
        //   bits [31..16] = cellX as int16
        //   bits [15..0]  = cellY as int16
        // Note this is NOT the same field order HoldGrid uses for its
        // own key; do not copy one to the other.
        //
        // This is the piece that makes the graph whole: the map covers
        // all ~16.6k navmeshes, not just the ~2.5k with resident forms.
        std::vector<InfoEntry> CollectInfos(RE::NavMeshInfoMap* infoMap)
        {
            std::vector<InfoEntry> entries;
            std::unordered_map<const RE::BSNavmeshInfo*, bool> seen;

            for (const auto& [key, arr] : infoMap->ckNavMeshInfoMap) {
                if (!arr) {
                    continue;
                }
                const auto worldSpace = static_cast<RE::FormID>(key >> 32);
                const auto cellX = static_cast<std::int16_t>((key >> 16) & 0xFFFF);
                const auto cellY = static_cast<std::int16_t>(key & 0xFFFF);
                for (const auto* info : *arr) {
                    if (!info || !seen.emplace(info, true).second) {
                        continue;
                    }
                    entries.push_back(InfoEntry{info, worldSpace, cellX, cellY});
                }
            }
            return entries;
        }

        // Read an entry's position. Prefers the calibrated point; falls
        // back to the centre of its cell. The sanity gate matters: a
        // point that isn't near the cell the engine filed it under means
        // we're reading the wrong bytes, and a cell centre is a far
        // better answer than a wild coordinate.
        RE::NiPoint3 PositionFor(const InfoEntry& entry, const InfoLayout& layout, bool& usedCalibrated)
        {
            const float cellCentreX = (static_cast<float>(entry.cellX) + 0.5f) * kCellUnits;
            const float cellCentreY = (static_cast<float>(entry.cellY) + 0.5f) * kCellUnits;

            if (layout.valid) {
                float f[3];
                const auto* base = reinterpret_cast<const std::uint8_t*>(entry.info) + layout.pointOffset;
                if (SafeReadBytes(base, f, sizeof(f)) && std::isfinite(f[0]) && std::isfinite(f[1])
                    && std::isfinite(f[2])) {
                    constexpr float kTolerance = 2.0f * kCellUnits;
                    if (std::fabs(f[0] - cellCentreX) <= kTolerance && std::fabs(f[1] - cellCentreY) <= kTolerance) {
                        usedCalibrated = true;
                        return RE::NiPoint3{f[0], f[1], f[2]};
                    }
                }
            }

            usedCalibrated = false;
            return RE::NiPoint3{cellCentreX, cellCentreY, 0.0f};
        }

        // ---------------- graph construction ------------------------

        void AddEdge(std::size_t a, std::size_t b)
        {
            if (a == b) {
                return;
            }
            auto& na = g_adjacency[a];
            if (std::find(na.begin(), na.end(), b) != na.end()) {
                return; // already linked
            }
            na.push_back(b);
            g_adjacency[b].push_back(a);
            ++g_edgeCount;
        }

        // ---------------- debug bitmap ------------------------------

        constexpr Bmp::RGB kWhite{255, 255, 255};
        // Darker than a "normal" gray on purpose: the edges now sit on
        // pastel hold fills rather than white, and a light gray washes
        // out against them.
        constexpr Bmp::RGB kGray{100, 100, 100};
        constexpr Bmp::RGB kBlack{0, 0, 0};

        // Background fill colors for holds. Deliberately pale — this is
        // a backdrop the graph is read against, so it has to stay well
        // clear of the black nodes and dark-gray edges drawn on top.
        //
        // These do NOT match HoldGrid's own debug bitmap palette, which
        // is saturated and assigned per-worldspace. Nothing compares the
        // two images side by side; the point here is to see the graph
        // against hold regions within a single image.
        constexpr std::array<Bmp::RGB, 12> kHoldPalette = {
            Bmp::RGB{255, 198, 198}, // red
            Bmp::RGB{196, 232, 196}, // green
            Bmp::RGB{198, 210, 246}, // blue
            Bmp::RGB{244, 236, 180}, // yellow
            Bmp::RGB{234, 200, 234}, // magenta
            Bmp::RGB{190, 230, 230}, // cyan
            Bmp::RGB{250, 216, 186}, // orange
            Bmp::RGB{250, 208, 226}, // pink
            Bmp::RGB{186, 216, 216}, // teal
            Bmp::RGB{208, 214, 228}, // slate
            Bmp::RGB{222, 222, 186}, // olive
            Bmp::RGB{222, 202, 182}, // brown
        };

        // Fallback for worldspaces with more holds than palette entries.
        // Biased bright so it stays in pastel territory.
        Bmp::RGB PastelHashColor(RE::FormID id)
        {
            auto channel = [](std::uint32_t v) { return static_cast<std::uint8_t>(186 + (v & 0xFF) * 70 / 255); };
            return Bmp::RGB{channel((id * 2654435761u) >> 16),
                            channel((id * 4055993439u) >> 16),
                            channel((id * 3266489917u) >> 16)};
        }

        struct Canvas
        {
            int width = 0;
            int height = 0;
            std::vector<Bmp::RGB> pixels;

            void Set(int x, int y, Bmp::RGB c)
            {
                if (x < 0 || y < 0 || x >= width || y >= height) {
                    return;
                }
                pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = c;
            }

            // Bresenham. Used for the gray edge strokes.
            void Line(int x0, int y0, int x1, int y1, Bmp::RGB c)
            {
                const int dx = std::abs(x1 - x0);
                const int dy = -std::abs(y1 - y0);
                const int sx = x0 < x1 ? 1 : -1;
                const int sy = y0 < y1 ? 1 : -1;
                int err = dx + dy;
                while (true) {
                    Set(x0, y0, c);
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
            }
        };

        void WriteWorldspaceBitmap(RE::TESWorldSpace* ws, RE::FormID wsID, const std::vector<std::size_t>& indices)
        {
            if (indices.empty()) {
                return;
            }

            float minX = std::numeric_limits<float>::max();
            float maxX = std::numeric_limits<float>::lowest();
            float minY = std::numeric_limits<float>::max();
            float maxY = std::numeric_limits<float>::lowest();
            for (const auto i : indices) {
                minX = std::min(minX, g_nodes[i].x);
                maxX = std::max(maxX, g_nodes[i].x);
                minY = std::min(minY, g_nodes[i].y);
                maxY = std::max(maxY, g_nodes[i].y);
            }

            // Pad by one cell so nodes on the extremes aren't clipped to
            // the border pixel.
            minX -= kCellUnits;
            maxX += kCellUnits;
            minY -= kCellUnits;
            maxY += kCellUnits;

            // Then widen to the worldspace's own extent.
            //
            // Framing to the nodes alone makes the graph touch all four
            // edges of the image, which reads as though the network
            // bounds the map — it looks like a border tracing rather
            // than roads running through the interior. Showing the whole
            // worldspace puts the network in context, with the hold fill
            // extending past it on every side.
            //
            // Many small worldspaces report degenerate (0,0)-(0,0)
            // bounds, so this only applies when the record actually
            // carries an extent; otherwise the node bounds stand. Either
            // way this only ever grows the frame, so nodes can't be
            // clipped.
            bool framedToWorldspace = false;
            if (ws) {
                const auto nwX = static_cast<int>(ws->worldMapData.nwCellX);
                const auto seX = static_cast<int>(ws->worldMapData.seCellX);
                const auto seY = static_cast<int>(ws->worldMapData.seCellY);
                const auto nwY = static_cast<int>(ws->worldMapData.nwCellY);
                if (seX > nwX && nwY > seY) {
                    minX = std::min(minX, static_cast<float>(nwX) * kCellUnits);
                    maxX = std::max(maxX, static_cast<float>(seX + 1) * kCellUnits);
                    minY = std::min(minY, static_cast<float>(seY) * kCellUnits);
                    maxY = std::max(maxY, static_cast<float>(nwY + 1) * kCellUnits);
                    framedToWorldspace = true;
                }
            }

            float unitsPerPixel = static_cast<float>(std::max(1, Settings::Get().travelGraphBitmapUnitsPerPixel));

            // Clamp the output size. A worldspace with far-flung outlier
            // navmeshes can otherwise ask for a multi-gigapixel canvas.
            constexpr int kMaxDimension = 4096;
            const float spanX = maxX - minX;
            const float spanY = maxY - minY;
            const float needed = std::max(spanX, spanY) / static_cast<float>(kMaxDimension);
            if (needed > unitsPerPixel) {
                unitsPerPixel = needed;
            }

            Canvas canvas;
            canvas.width = std::max(1, static_cast<int>(spanX / unitsPerPixel) + 1);
            canvas.height = std::max(1, static_cast<int>(spanY / unitsPerPixel) + 1);
            canvas.pixels.assign(static_cast<std::size_t>(canvas.width) * static_cast<std::size_t>(canvas.height),
                                 kWhite);

            // Background: HoldGrid's partition over the same area, so
            // the graph can be judged against hold regions rather than
            // floating on white.
            //
            // Resolved per CELL, not per pixel: the image is ~1.1M
            // pixels but only ~10k cells, and every HoldGrid lookup
            // takes a mutex. Cells are looked up once into a local grid
            // and pixels index into that.
            if (ws) {
                const int cellMinX = static_cast<int>(std::floor(minX / kCellUnits));
                const int cellMaxX = static_cast<int>(std::floor(maxX / kCellUnits));
                const int cellMinY = static_cast<int>(std::floor(minY / kCellUnits));
                const int cellMaxY = static_cast<int>(std::floor(maxY / kCellUnits));
                const int cellW = cellMaxX - cellMinX + 1;
                const int cellH = cellMaxY - cellMinY + 1;

                if (cellW > 0 && cellH > 0) {
                    std::vector<RE::FormID> holdByCell(
                        static_cast<std::size_t>(cellW) * static_cast<std::size_t>(cellH), 0);
                    std::vector<RE::FormID> distinctHolds;
                    for (int cy = 0; cy < cellH; ++cy) {
                        for (int cx = 0; cx < cellW; ++cx) {
                            const auto hold = HoldGrid::LookupCell(
                                ws, static_cast<std::int16_t>(cellMinX + cx), static_cast<std::int16_t>(cellMinY + cy));
                            holdByCell[static_cast<std::size_t>(cy) * static_cast<std::size_t>(cellW)
                                       + static_cast<std::size_t>(cx)] = hold;
                            if (hold != 0
                                && std::find(distinctHolds.begin(), distinctHolds.end(), hold) == distinctHolds.end()) {
                                distinctHolds.push_back(hold);
                            }
                        }
                    }

                    // Sort so palette assignment is stable across runs.
                    std::sort(distinctHolds.begin(), distinctHolds.end());
                    std::unordered_map<RE::FormID, Bmp::RGB> colorByHold;
                    for (std::size_t i = 0; i < distinctHolds.size(); ++i) {
                        colorByHold[distinctHolds[i]] =
                            (i < kHoldPalette.size()) ? kHoldPalette[i] : PastelHashColor(distinctHolds[i]);
                    }

                    for (int py = 0; py < canvas.height; ++py) {
                        const float worldY = maxY - static_cast<float>(py) * unitsPerPixel;
                        const int cy = static_cast<int>(std::floor(worldY / kCellUnits)) - cellMinY;
                        if (cy < 0 || cy >= cellH) {
                            continue;
                        }
                        for (int px = 0; px < canvas.width; ++px) {
                            const float worldX = minX + static_cast<float>(px) * unitsPerPixel;
                            const int cx = static_cast<int>(std::floor(worldX / kCellUnits)) - cellMinX;
                            if (cx < 0 || cx >= cellW) {
                                continue;
                            }
                            const auto hold = holdByCell[static_cast<std::size_t>(cy) * static_cast<std::size_t>(cellW)
                                                         + static_cast<std::size_t>(cx)];
                            if (hold == 0) {
                                continue; // unassigned stays white
                            }
                            canvas.Set(px, py, colorByHold[hold]);
                        }
                    }

                    // Log the palette so the image can be decoded.
                    logger::info("TravelGraph: hold background palette for '{}':",
                                 ws->GetFormEditorID() ? ws->GetFormEditorID() : "?");
                    for (const auto hold : distinctHolds) {
                        auto* form = RE::TESForm::LookupByID(hold);
                        auto* loc = form ? form->As<RE::BGSLocation>() : nullptr;
                        const char* edid = loc ? loc->GetFormEditorID() : nullptr;
                        const char* full = loc ? loc->GetFullName() : nullptr;
                        const auto& c = colorByHold[hold];
                        logger::info("TravelGraph:   [0x{:08X}] EditorID='{}' FullName='{}' RGB=({},{},{})",
                                     hold,
                                     (edid && *edid) ? edid : "?",
                                     (full && *full) ? full : "?",
                                     c.r,
                                     c.g,
                                     c.b);
                    }
                }
            }

            // World -> image. Image y=0 is the TOP, and +Y is north in
            // Skyrim, so y flips: north ends up at the top of the image.
            auto toPixel = [&](const Node& n) {
                return std::pair<int, int>{static_cast<int>((n.x - minX) / unitsPerPixel),
                                           static_cast<int>((maxY - n.y) / unitsPerPixel)};
            };

            // Edges first so node dots draw on top of them.
            for (const auto i : indices) {
                const auto [ax, ay] = toPixel(g_nodes[i]);
                for (const auto j : g_adjacency[i]) {
                    if (j < i) {
                        continue; // each undirected edge once
                    }
                    if (g_nodes[j].worldSpace != g_nodes[i].worldSpace) {
                        continue; // don't draw across worldspaces
                    }
                    const auto [bx, by] = toPixel(g_nodes[j]);
                    canvas.Line(ax, ay, bx, by, kGray);
                }
            }

            for (const auto i : indices) {
                const auto [px, py] = toPixel(g_nodes[i]);
                // 3x3 so nodes read clearly against the hold fills.
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        canvas.Set(px + dx, py + dy, kBlack);
                    }
                }
            }

            auto logsFolder = SKSE::log::log_directory();
            if (!logsFolder) {
                logger::warn("TravelGraph: cannot write bitmap - SKSE log_directory unavailable");
                return;
            }
            const char* wsEdid = ws ? ws->GetFormEditorID() : nullptr;
            const std::string wsName =
                (wsEdid && *wsEdid) ? std::string{wsEdid} : std::format("{:08X}", static_cast<std::uint32_t>(wsID));
            const auto path = *logsFolder / ("NarrativeEngine_TravelGraph_" + wsName + ".bmp");

            const bool ok = Bmp::Write24(path, canvas.width, canvas.height, [&](int x, int y) {
                return canvas.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas.width)
                                     + static_cast<std::size_t>(x)];
            });

            if (ok) {
                logger::info("TravelGraph: wrote bitmap '{}' ({}x{}, {} node(s), {:.0f} units/pixel, framed to {})",
                             path.string(),
                             canvas.width,
                             canvas.height,
                             indices.size(),
                             unitsPerPixel,
                             framedToWorldspace ? "worldspace extent" : "graph extent");
            } else {
                logger::warn("TravelGraph: failed to write bitmap '{}'", path.string());
            }
        }

        void WriteAllBitmaps()
        {
            // Group nodes by worldspace. Worldspaces with no nodes never
            // appear here, which is the "skip empty worldspaces" rule.
            std::unordered_map<RE::FormID, std::vector<std::size_t>> byWorldspace;
            for (std::size_t i = 0; i < g_nodes.size(); ++i) {
                byWorldspace[g_nodes[i].worldSpace].push_back(i);
            }

            for (const auto& [wsID, indices] : byWorldspace) {
                auto* form = RE::TESForm::LookupByID(wsID);
                auto* ws = form ? form->As<RE::TESWorldSpace>() : nullptr;
                WriteWorldspaceBitmap(ws, wsID, indices);
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

        if (!Settings::Get().travelGraphEnabled) {
            logger::info("TravelGraph: disabled by bTravelGraphEnabled; skipping build");
            return;
        }

        const auto startTime = std::chrono::steady_clock::now();

        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            logger::warn("TravelGraph: TESDataHandler unavailable; graph disabled");
            return;
        }

        const FormScan scan = ScanForms(dh);
        auto* infoMap = scan.infoMap;
        if (!infoMap) {
            logger::warn("TravelGraph: no NavMeshInfoMap (NAVI) form found; graph disabled");
            return;
        }

        // Step 1 — calibration samples.
        //
        // Only navmeshes with a resident NavMesh form can serve here,
        // because only those give us a position we already trust. That's
        // a minority of the total and geographically clustered around
        // whatever was loaded, which is fine: a struct layout doesn't
        // vary by region, so a clustered sample still measures it.
        std::vector<CalibrationSample> samples;
        for (auto* navMesh : scan.navMeshes) {
            if (!navMesh) {
                continue;
            }
            auto* cell = navMesh->GetSaveParentCell();
            if (!cell || cell->IsInteriorCell()) {
                continue;
            }
            const auto& lo = navMesh->meshGrid.gridBoundsMin;
            const auto& hi = navMesh->meshGrid.gridBoundsMax;
            if (hi.x < lo.x || hi.y < lo.y) {
                continue;
            }
            if (lo.x == 0.0f && lo.y == 0.0f && hi.x == 0.0f && hi.y == 0.0f) {
                continue; // no geometry loaded for this one
            }
            auto* info = infoMap->GetNavmeshInfo(navMesh->GetFormID());
            if (!info) {
                continue;
            }
            samples.push_back(
                CalibrationSample{info, navMesh->GetFormID(), (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f});
        }

        const InfoLayout layout = CalibrateInfoLayout(samples, Settings::Get().travelGraphLogCalibration);

        // Step 2 — a node for every navmesh the engine knows about.
        const auto entries = CollectInfos(infoMap);
        if (entries.empty()) {
            logger::warn("TravelGraph: ckNavMeshInfoMap is empty; graph disabled");
            return;
        }

        std::unordered_map<const RE::BSNavmeshInfo*, std::size_t> infoToNode;
        infoToNode.reserve(entries.size());
        g_nodes.reserve(entries.size());
        g_adjacency.reserve(entries.size());

        std::size_t calibratedPositions = 0;
        for (const auto& entry : entries) {
            bool usedCalibrated = false;
            const auto pos = PositionFor(entry, layout, usedCalibrated);
            if (usedCalibrated) {
                ++calibratedPositions;
            }

            RE::FormID navMeshID = 0;
            if (layout.hasFormId) {
                std::uint32_t id = 0;
                const auto* base = reinterpret_cast<const std::uint8_t*>(entry.info) + layout.formIdOffset;
                if (SafeReadBytes(base, &id, sizeof(id))) {
                    navMeshID = id;
                }
            }

            infoToNode.emplace(entry.info, g_nodes.size());
            g_nodes.push_back(Node{navMeshID, entry.worldSpace, pos.x, pos.y, pos.z});
            g_adjacency.emplace_back();
        }

        logger::info("TravelGraph: {} navmesh(es) known to NAVI; {} positioned from the calibrated point, "
                     "{} fell back to cell centre",
                     entries.size(),
                     calibratedPositions,
                     entries.size() - calibratedPositions);

        // Step 3 — edges from the precomputed preferred-path chains.
        // Each entry of allPaths is an ordered sequence of navmeshes
        // forming one long-distance route; consecutive entries are an
        // edge. Chains overlap where routes share road, and because the
        // dedupe in AddEdge is on node identity, the overlap becomes a
        // real junction rather than a parallel corridor.
        std::size_t chains = 0;
        std::size_t unmapped = 0;
        for (auto* chain : infoMap->allPaths) {
            if (!chain || chain->empty()) {
                continue;
            }
            ++chains;

            std::size_t previous = kInvalidNode;
            for (const auto* info : *chain) {
                const auto it = info ? infoToNode.find(info) : infoToNode.end();
                if (it == infoToNode.end()) {
                    ++unmapped;
                    // Break the run: we don't know where this hop was, so
                    // linking across it would invent a road segment.
                    previous = kInvalidNode;
                    continue;
                }
                if (previous != kInvalidNode) {
                    AddEdge(previous, it->second);
                }
                previous = it->second;
            }
        }

        // Step 4 — drop navmeshes that no chain ever touched.
        //
        // Step 2 admits every navmesh because we can't know which ones
        // matter until the chains have been walked, and most don't.
        // Keeping the rest would make FindNearestNode answer "nearest
        // patch of ground" instead of "nearest road", and would bury the
        // road network under a solid field of dots in the debug bitmap.
        const std::size_t preCompactionNodes = g_nodes.size();
        {
            std::vector<std::size_t> remap(g_nodes.size(), kInvalidNode);
            std::vector<Node> keptNodes;
            std::vector<std::vector<std::size_t>> keptAdjacency;
            for (std::size_t i = 0; i < g_nodes.size(); ++i) {
                if (g_adjacency[i].empty()) {
                    continue;
                }
                remap[i] = keptNodes.size();
                keptNodes.push_back(g_nodes[i]);
                keptAdjacency.push_back(std::move(g_adjacency[i]));
            }
            for (auto& neighbors : keptAdjacency) {
                for (auto& n : neighbors) {
                    n = remap[n]; // every neighbor has degree >= 1, so never kInvalidNode
                }
            }
            g_nodes = std::move(keptNodes);
            g_adjacency = std::move(keptAdjacency);
        }

        const auto elapsed = std::chrono::steady_clock::now() - startTime;
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        logger::info("TravelGraph: built {} node(s), {} edge(s) from {} preferred-path chain(s) ({}ms)",
                     g_nodes.size(),
                     g_edgeCount,
                     chains,
                     elapsedMs);
        logger::info("TravelGraph:   unmapped chain entries={}; candidates={} kept={}",
                     unmapped,
                     preCompactionNodes,
                     g_nodes.size());

        if (g_edgeCount == 0) {
            logger::warn("TravelGraph: no edges resolved despite {} chain(s) — chain entries are not matching "
                         "ckNavMeshInfoMap pointers",
                         chains);
        }

        if (Settings::Get().travelGraphDebugBitmap) {
            WriteAllBitmaps();
        }
    }

    std::size_t NodeCount()
    {
        std::scoped_lock lock(g_mutex);
        return g_nodes.size();
    }

    std::size_t EdgeCount()
    {
        std::scoped_lock lock(g_mutex);
        return g_edgeCount;
    }

    const Node* GetNode(std::size_t index)
    {
        std::scoped_lock lock(g_mutex);
        return index < g_nodes.size() ? &g_nodes[index] : nullptr;
    }

    std::size_t FindNearestNode(RE::FormID worldSpace, float x, float y)
    {
        std::scoped_lock lock(g_mutex);
        std::size_t best = kInvalidNode;
        float bestDistSq = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < g_nodes.size(); ++i) {
            const auto& n = g_nodes[i];
            if (n.worldSpace != worldSpace) {
                continue;
            }
            const float dx = n.x - x;
            const float dy = n.y - y;
            const float distSq = dx * dx + dy * dy;
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                best = i;
            }
        }
        return best;
    }

    std::vector<float> DistanceField(std::size_t source)
    {
        std::scoped_lock lock(g_mutex);
        if (source >= g_nodes.size()) {
            return {};
        }

        constexpr float kInf = std::numeric_limits<float>::max();
        std::vector<float> dist(g_nodes.size(), kInf);
        std::vector<bool> settled(g_nodes.size(), false);

        using Entry = std::pair<float, std::size_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;
        dist[source] = 0.0f;
        frontier.emplace(0.0f, source);

        while (!frontier.empty()) {
            const auto [d, current] = frontier.top();
            frontier.pop();
            if (settled[current]) {
                continue;
            }
            settled[current] = true;
            for (const auto next : g_adjacency[current]) {
                if (settled[next]) {
                    continue;
                }
                const float dx = g_nodes[next].x - g_nodes[current].x;
                const float dy = g_nodes[next].y - g_nodes[current].y;
                const float step = std::sqrt(dx * dx + dy * dy);
                if (d + step < dist[next]) {
                    dist[next] = d + step;
                    frontier.emplace(dist[next], next);
                }
            }
        }
        return dist;
    }

    std::vector<std::size_t> FindPath(std::size_t from, std::size_t to)
    {
        std::scoped_lock lock(g_mutex);
        if (from >= g_nodes.size() || to >= g_nodes.size()) {
            return {};
        }
        if (from == to) {
            return {from};
        }

        constexpr float kInf = std::numeric_limits<float>::max();
        std::vector<float> dist(g_nodes.size(), kInf);
        std::vector<std::size_t> previous(g_nodes.size(), kInvalidNode);
        std::vector<bool> settled(g_nodes.size(), false);

        using Entry = std::pair<float, std::size_t>; // (distance, node)
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;

        dist[from] = 0.0f;
        frontier.emplace(0.0f, from);

        while (!frontier.empty()) {
            const auto [d, current] = frontier.top();
            frontier.pop();
            if (settled[current]) {
                continue; // stale queue entry
            }
            settled[current] = true;
            if (current == to) {
                break;
            }
            for (const auto next : g_adjacency[current]) {
                if (settled[next]) {
                    continue;
                }
                const float dx = g_nodes[next].x - g_nodes[current].x;
                const float dy = g_nodes[next].y - g_nodes[current].y;
                const float step = std::sqrt(dx * dx + dy * dy);
                if (d + step < dist[next]) {
                    dist[next] = d + step;
                    previous[next] = current;
                    frontier.emplace(dist[next], next);
                }
            }
        }

        if (dist[to] == kInf) {
            return {}; // disconnected
        }

        std::vector<std::size_t> path;
        for (std::size_t at = to; at != kInvalidNode; at = previous[at]) {
            path.push_back(at);
            if (at == from) {
                break;
            }
        }
        std::reverse(path.begin(), path.end());
        return path;
    }
} // namespace NarrativeEngine::TravelGraph
