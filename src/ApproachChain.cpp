#include <ApproachChain.h>

#include <logger.h>
#include <Settings.h>
#include <TravelGraph.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NarrativeEngine::ApproachChain
{
    namespace
    {
        constexpr float kInf = std::numeric_limits<float>::max();
        constexpr float kCellUnits = 4096.0f;

        // How far above the height its own line interpolates a synthetic
        // point outside the loaded grid is placed.
        //
        // Nothing can ground it: Step 1 of Phase 16 established that
        // TES::GetLandHeight returns false for every position outside the
        // attached grid (and writes -2048 into the out parameter while
        // doing so), so there is no landscape to read. An actor warped
        // there stays stationary until background travel carries them
        // into the grid and does not interact with terrain in the
        // meantime, so erring upward by one actor height costs nothing
        // and erring downward risks starting them inside a hill.
        //
        // Points INSIDE the grid keep their interpolated height, because
        // the arrival search re-grounds those against real navmesh.
        constexpr float kSyntheticLiftUnits = 128.0f;

        float Dist(const RE::NiPoint3& a, const RE::NiPoint3& b)
        {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        struct Node
        {
            RE::NiPoint3 pos{};
            PointClass cls = PointClass::Direct;
            int difficulty = 1;
            std::size_t fineNode = FineRoads::kInvalidNode;
        };

        // The composite graph. Undirected, and small enough that an
        // adjacency list of indices is the whole of what it needs — the
        // coarse skeleton is ~620 nodes, a loaded fine graph ~110, and a
        // direct line across the province at 512-unit spacing ~200.
        struct Graph
        {
            std::vector<Node> nodes;
            std::vector<std::vector<std::size_t>> adjacency;

            std::size_t Add(const RE::NiPoint3& pos,
                            PointClass cls,
                            int difficulty,
                            std::size_t fineNode = FineRoads::kInvalidNode)
            {
                nodes.push_back(Node{pos, cls, difficulty, fineNode});
                adjacency.emplace_back();
                return nodes.size() - 1;
            }

            void Link(std::size_t a, std::size_t b)
            {
                if (a == b || a >= nodes.size() || b >= nodes.size()) {
                    return;
                }
                // Deduplicated, because the two bridges can land on the
                // same pair and a doubled edge would be walked twice by
                // anything summing cost over the graph.
                if (std::find(adjacency[a].begin(), adjacency[a].end(), b) != adjacency[a].end()) {
                    return;
                }
                adjacency[a].push_back(b);
                adjacency[b].push_back(a);
            }

            [[nodiscard]] std::size_t EdgeEnds() const
            {
                std::size_t ends = 0;
                for (const auto& row : adjacency) {
                    ends += row.size();
                }
                return ends;
            }
        };

        // Lay `cls` points every `spacing` units along the open segment
        // between two nodes already in the graph, and link the run.
        //
        // The endpoints are untouched — they belong to whatever put them
        // there — so this fills the inside of a segment and nothing else.
        void LayLine(Graph& graph,
                     const LoadedGrid& grid,
                     RE::FormID worldSpace,
                     std::size_t from,
                     std::size_t to,
                     PointClass cls,
                     int difficulty,
                     float spacing)
        {
            if (from >= graph.nodes.size() || to >= graph.nodes.size() || from == to) {
                return;
            }
            const auto start = graph.nodes[from].pos;
            const auto end = graph.nodes[to].pos;
            const float length = Dist(start, end);
            if (!(length > spacing)) {
                // Closer together than one step: the segment is one edge
                // and there is no room for a point inside it.
                graph.Link(from, to);
                return;
            }

            std::size_t previous = from;
            for (float travelled = spacing; travelled < length; travelled += spacing) {
                const float t = travelled / length;
                RE::NiPoint3 pos{
                    start.x + (end.x - start.x) * t,
                    start.y + (end.y - start.y) * t,
                    start.z + (end.z - start.z) * t,
                };
                if (!grid.Contains(worldSpace, pos)) {
                    pos.z += kSyntheticLiftUnits;
                }
                const auto added = graph.Add(pos, cls, difficulty);
                graph.Link(previous, added);
                previous = added;
            }
            graph.Link(previous, to);
        }

        // Index of the graph node nearest `target` among `candidates`.
        std::size_t NearestOf(const Graph& graph,
                              const std::vector<std::size_t>& candidates,
                              const RE::NiPoint3& target)
        {
            std::size_t best = FineRoads::kInvalidNode;
            float bestDist = kInf;
            for (const auto index : candidates) {
                const float d = Dist(graph.nodes[index].pos, target);
                if (d < bestDist) {
                    bestDist = d;
                    best = index;
                }
            }
            return best;
        }

        // Which fine nodes belong to the network holding the node closest
        // to the player, plus how many networks there were.
        //
        // The loaded grid can hold several unconnected pieces of road —
        // two valleys either side of a ridge, a bridgeless river. Only one
        // is worth putting in the graph: an unbridged network is an island
        // the search can never enter, so adding the others is cost without
        // reach. The one holding the closest node to the player is the one
        // a visit cares about, because every arrival point is chosen by
        // walking outward from the player.
        struct FineSelection
        {
            std::vector<bool> selected;
            std::size_t networkCount = 0;
            std::size_t selectedSize = 0;
        };

        FineSelection SelectFineNetwork(const FineRoads::Graph& fine, const RE::NiPoint3& playerPos)
        {
            FineSelection out;
            out.selected.assign(fine.nodes.size(), false);
            if (fine.nodes.empty()) {
                return out;
            }

            // Label every node with its component.
            std::vector<std::size_t> component(fine.nodes.size(), FineRoads::kInvalidNode);
            std::vector<std::size_t> stack;
            for (std::size_t seed = 0; seed < fine.nodes.size(); ++seed) {
                if (component[seed] != FineRoads::kInvalidNode) {
                    continue;
                }
                const std::size_t label = out.networkCount++;
                stack.push_back(seed);
                component[seed] = label;
                while (!stack.empty()) {
                    const auto at = stack.back();
                    stack.pop_back();
                    for (const auto next : fine.adjacency[at]) {
                        if (next < component.size() && component[next] == FineRoads::kInvalidNode) {
                            component[next] = label;
                            stack.push_back(next);
                        }
                    }
                }
            }

            std::size_t closest = 0;
            float bestDist = kInf;
            for (std::size_t i = 0; i < fine.nodes.size(); ++i) {
                const auto& n = fine.nodes[i];
                const float d = Dist(RE::NiPoint3{n.x, n.y, n.z}, playerPos);
                if (d < bestDist) {
                    bestDist = d;
                    closest = i;
                }
            }

            const auto chosen = component[closest];
            for (std::size_t i = 0; i < fine.nodes.size(); ++i) {
                if (component[i] == chosen) {
                    out.selected[i] = true;
                    ++out.selectedSize;
                }
            }
            return out;
        }

        struct SearchResult
        {
            std::vector<std::size_t> path; // start -> goal
            float cost = 0.0f;
            bool found = false;
        };

        // Cost of traversing the edge between two nodes: its length times
        // the HIGHER of its two endpoints' difficulties.
        //
        // `max` rather than an average, so one expensive node is
        // genuinely expensive to pass through instead of being diluted by
        // a cheap neighbour. An average would make a long cheap approach
        // to a single costly node far more attractive than intended,
        // which is the one behaviour this cost function exists to
        // prevent.
        float EdgeCost(const Graph& graph, std::size_t a, std::size_t b)
        {
            const int difficulty = std::max(graph.nodes[a].difficulty, graph.nodes[b].difficulty);
            return Dist(graph.nodes[a].pos, graph.nodes[b].pos) * static_cast<float>(difficulty);
        }

        // Walk `previous` back from the goal. Returns empty when the trail
        // does not reach the start, because reporting a partial route
        // would be worse than reporting none.
        std::vector<std::size_t> Reconstruct(const std::vector<std::size_t>& previous,
                                             std::size_t start,
                                             std::size_t goal)
        {
            std::vector<std::size_t> path;
            for (std::size_t at = goal;; at = previous[at]) {
                path.push_back(at);
                if (at == start) {
                    break;
                }
                if (previous[at] == FineRoads::kInvalidNode) {
                    return {};
                }
            }
            std::reverse(path.begin(), path.end());
            return path;
        }

        // Plain Dijkstra over the same graph, with no heuristic. Only the
        // test suite's oracle calls this; the module itself uses the
        // A-star below.
        SearchResult Dijkstra(const Graph& graph, std::size_t start, std::size_t goal)
        {
            SearchResult out;
            if (start >= graph.nodes.size() || goal >= graph.nodes.size()) {
                return out;
            }
            std::vector<float> dist(graph.nodes.size(), kInf);
            std::vector<std::size_t> previous(graph.nodes.size(), FineRoads::kInvalidNode);
            std::vector<bool> settled(graph.nodes.size(), false);

            using Entry = std::pair<float, std::size_t>;
            std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;
            dist[start] = 0.0f;
            frontier.emplace(0.0f, start);
            while (!frontier.empty()) {
                const auto [d, current] = frontier.top();
                frontier.pop();
                if (settled[current]) {
                    continue;
                }
                settled[current] = true;
                for (const auto next : graph.adjacency[current]) {
                    const float tentative = d + EdgeCost(graph, current, next);
                    if (tentative < dist[next]) {
                        dist[next] = tentative;
                        previous[next] = current;
                        frontier.emplace(tentative, next);
                    }
                }
            }
            if (dist[goal] == kInf) {
                return out;
            }
            out.path = Reconstruct(previous, start, goal);
            out.found = !out.path.empty();
            out.cost = dist[goal];
            return out;
        }

        // A-star from `start` to `goal`, with straight-line distance to the
        // goal as the heuristic.
        //
        // That heuristic underestimates the true cost — and so returns the
        // cheapest route rather than merely a route — only while no edge
        // costs less than its own length, which is why difficulty 1 is the
        // floor of the scale and Settings clamps to it.
        SearchResult AStar(const Graph& graph, std::size_t start, std::size_t goal)
        {
            SearchResult out;
            if (start >= graph.nodes.size() || goal >= graph.nodes.size()) {
                return out;
            }

            const auto& goalPos = graph.nodes[goal].pos;
            std::vector<float> gScore(graph.nodes.size(), kInf);
            std::vector<std::size_t> previous(graph.nodes.size(), FineRoads::kInvalidNode);
            std::vector<bool> settled(graph.nodes.size(), false);

            using Entry = std::pair<float, std::size_t>;
            std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;
            gScore[start] = 0.0f;
            frontier.emplace(Dist(graph.nodes[start].pos, goalPos), start);

            while (!frontier.empty()) {
                const auto [_, current] = frontier.top();
                frontier.pop();
                if (settled[current]) {
                    continue;
                }
                if (current == goal) {
                    break;
                }
                settled[current] = true;
                for (const auto next : graph.adjacency[current]) {
                    if (settled[next]) {
                        continue;
                    }
                    const float tentative = gScore[current] + EdgeCost(graph, current, next);
                    if (tentative < gScore[next]) {
                        gScore[next] = tentative;
                        previous[next] = current;
                        frontier.emplace(tentative + Dist(graph.nodes[next].pos, goalPos), next);
                    }
                }
            }

            if (gScore[goal] == kInf) {
                return out;
            }
            out.path = Reconstruct(previous, start, goal);
            out.found = !out.path.empty();
            out.cost = gScore[goal];
            return out;
        }

        // Everything `Build` puts into the graph, and where the two
        // endpoints ended up in it.
        struct Assembly
        {
            Graph graph;
            std::size_t visitorNode = FineRoads::kInvalidNode;
            std::size_t playerNode = FineRoads::kInvalidNode;
            std::size_t coarseCount = 0;
            std::size_t fineSelected = 0;
            std::size_t fineNetworks = 0;
            bool ok = false;
        };

        Assembly Assemble(const LoadedGrid& grid,
                          RE::FormID worldSpace,
                          const RE::NiPoint3& visitorOrigin,
                          const RE::NiPoint3& playerPos)
        {
            Assembly out;
            if (worldSpace == 0) {
                return out;
            }

            const auto& cfg = Settings::Get();
            const float spacing = static_cast<float>(std::max(1, cfg.visitChainBridgeSpacingUnits));
            const int coarseDifficulty = std::max(1, cfg.visitChainDifficultyCoarse);
            const int fineDifficulty = std::max(1, cfg.visitChainDifficultyFine);
            const int connectorDifficulty = std::max(1, cfg.visitChainDifficultyConnector);
            const int directDifficulty = std::max(1, cfg.visitChainDifficultyDirect);

            Graph& graph = out.graph;

            // ---- difficulty 1: the coarse skeleton ------------------
            //
            // Restricted to this worldspace. No coarse edge crosses
            // between worldspaces, so filtering the nodes leaves the
            // edges coherent.
            std::unordered_map<std::size_t, std::size_t> coarseToGraph;
            std::vector<std::size_t> coarseNodes;
            for (std::size_t i = 0; i < TravelGraph::NodeCount(); ++i) {
                const auto* node = TravelGraph::GetNode(i);
                if (!node || node->worldSpace != worldSpace) {
                    continue;
                }
                const auto added =
                    graph.Add(RE::NiPoint3{node->x, node->y, node->z}, PointClass::Coarse, coarseDifficulty);
                coarseToGraph.emplace(i, added);
                coarseNodes.push_back(added);
            }
            for (const auto& [coarseIndex, graphIndex] : coarseToGraph) {
                for (const auto neighbor : TravelGraph::Neighbors(coarseIndex)) {
                    const auto it = coarseToGraph.find(neighbor);
                    if (it != coarseToGraph.end()) {
                        graph.Link(graphIndex, it->second);
                    }
                }
            }
            out.coarseCount = coarseNodes.size();

            // ---- difficulty 2: the selected fine network ------------
            const auto fine = FineRoads::Snapshot();
            const bool fineUsable = !fine.nodes.empty() && fine.worldSpace == worldSpace;
            const auto selection = fineUsable ? SelectFineNetwork(fine, playerPos) : FineSelection{};
            out.fineSelected = selection.selectedSize;
            out.fineNetworks = selection.networkCount;

            std::unordered_map<std::size_t, std::size_t> fineToGraph;
            std::vector<std::size_t> fineNodes;
            if (fineUsable) {
                for (std::size_t i = 0; i < fine.nodes.size(); ++i) {
                    if (!selection.selected[i]) {
                        continue;
                    }
                    const auto& n = fine.nodes[i];
                    const auto added = graph.Add(RE::NiPoint3{n.x, n.y, n.z}, PointClass::Fine, fineDifficulty, i);
                    fineToGraph.emplace(i, added);
                    fineNodes.push_back(added);
                }
                for (const auto& [fineIndex, graphIndex] : fineToGraph) {
                    for (const auto neighbor : fine.adjacency[fineIndex]) {
                        const auto it = fineToGraph.find(neighbor);
                        if (it != fineToGraph.end()) {
                            graph.Link(graphIndex, it->second);
                        }
                    }
                }
            }

            // ---- difficulty 3: the endpoints and their connectors ---
            out.playerNode = graph.Add(playerPos, PointClass::Connector, connectorDifficulty);
            out.visitorNode = graph.Add(visitorOrigin, PointClass::Connector, connectorDifficulty);

            // The player reaches the road network directly: onto the
            // fine network they are standing beside, and onto the coarse
            // skeleton so a cross-province route has somewhere to arrive.
            //
            // Laid rather than linked. A single edge costs the same as a
            // laid one — same length, same difficulty at both ends, and
            // LayLine falls back to one edge when the gap is under a
            // spacing — so this does not re-price anything. What it adds
            // is points along the way, and without them the nearest
            // candidate past the distance floor is the first road node,
            // however far out that is. Measured in a session before this
            // existed: a player standing where no fine graph loads got an
            // arrival 20,960 units away, because the only thing between
            // them and their nearest coarse node was one edge.
            const auto playerFineAttach = NearestOf(graph, fineNodes, playerPos);
            if (playerFineAttach != FineRoads::kInvalidNode) {
                LayLine(graph,
                        grid,
                        worldSpace,
                        out.playerNode,
                        playerFineAttach,
                        PointClass::Connector,
                        connectorDifficulty,
                        spacing);
            }
            const auto playerCoarseAttach = NearestOf(graph, coarseNodes, playerPos);
            if (playerCoarseAttach != FineRoads::kInvalidNode) {
                LayLine(graph,
                        grid,
                        worldSpace,
                        out.playerNode,
                        playerCoarseAttach,
                        PointClass::Connector,
                        connectorDifficulty,
                        spacing);
            }

            // The visitor reaches the coarse skeleton the same way. They
            // do NOT get a cheap connector onto the fine network — that
            // is what the direct line below is for, and a connector would
            // undercut it.
            const auto visitorCoarseAttach = NearestOf(graph, coarseNodes, visitorOrigin);
            if (visitorCoarseAttach != FineRoads::kInvalidNode) {
                LayLine(graph,
                        grid,
                        worldSpace,
                        out.visitorNode,
                        visitorCoarseAttach,
                        PointClass::Connector,
                        connectorDifficulty,
                        spacing);
            }

            // The bridge: coarse skeleton to the selected fine network,
            // at whichever pair of nodes is cheapest to join. This is the
            // gap RoadRoute measures and then throws away, and filling it
            // is what lets a visitor be placed on a connected point just
            // outside the loaded grid rather than the visit declining.
            if (!fineNodes.empty() && !coarseNodes.empty()) {
                std::size_t bestFine = FineRoads::kInvalidNode;
                std::size_t bestCoarse = FineRoads::kInvalidNode;
                float bestDist = kInf;
                for (const auto fineIndex : fineNodes) {
                    for (const auto coarseIndex : coarseNodes) {
                        const float d = Dist(graph.nodes[fineIndex].pos, graph.nodes[coarseIndex].pos);
                        if (d < bestDist) {
                            bestDist = d;
                            bestFine = fineIndex;
                            bestCoarse = coarseIndex;
                        }
                    }
                }
                if (bestFine != FineRoads::kInvalidNode) {
                    LayLine(graph,
                            grid,
                            worldSpace,
                            bestFine,
                            bestCoarse,
                            PointClass::Connector,
                            connectorDifficulty,
                            spacing);
                }
            }

            // ---- difficulty 4: the two direct lines ----------------
            //
            // The first is the useful one: it lets a visitor who is
            // off-road but close cut straight to the road near the player
            // and finish on cheap ground. The second is the connectivity
            // guarantee, and the only segment that exists when no fine
            // network is loaded.
            const auto visitorFineAttach = NearestOf(graph, fineNodes, visitorOrigin);
            if (visitorFineAttach != FineRoads::kInvalidNode) {
                LayLine(graph,
                        grid,
                        worldSpace,
                        out.visitorNode,
                        visitorFineAttach,
                        PointClass::Direct,
                        directDifficulty,
                        spacing);
            }
            LayLine(graph,
                    grid,
                    worldSpace,
                    out.visitorNode,
                    out.playerNode,
                    PointClass::Direct,
                    directDifficulty,
                    spacing);

            out.ok = true;
            return out;
        }
    } // namespace

    const char* PointClassName(PointClass cls)
    {
        switch (cls) {
        case PointClass::Coarse:
            return "coarse";
        case PointClass::Fine:
            return "fine";
        case PointClass::Connector:
            return "connector";
        case PointClass::Direct:
            return "direct";
        }
        return "unknown";
    }

    bool LoadedGrid::Contains(RE::FormID ws, const RE::NiPoint3& pos) const
    {
        if (!valid || ws == 0 || ws != worldSpace) {
            return false;
        }
        const auto cx = static_cast<std::int32_t>(std::floor(pos.x / kCellUnits));
        const auto cy = static_cast<std::int32_t>(std::floor(pos.y / kCellUnits));
        return cx >= minCellX && cx <= maxCellX && cy >= minCellY && cy <= maxCellY;
    }

    LoadedGrid ReadLoadedGrid(const MainThread::Token&)
    {
        LoadedGrid out;
        auto* tes = RE::TES::GetSingleton();
        if (!tes || tes->interiorCell) {
            return out; // indoors: no exterior grid, which is a normal answer
        }
        auto* grid = tes->gridCells;
        if (!grid || !grid->cells) {
            return out;
        }

        bool first = true;
        for (std::uint32_t gx = 0; gx < grid->length; ++gx) {
            for (std::uint32_t gy = 0; gy < grid->length; ++gy) {
                auto* cell = grid->GetCell(gx, gy);
                if (!cell || cell->IsInteriorCell() || !cell->IsAttached()) {
                    continue; // in the grid but not loaded yet
                }
                auto* coords = cell->GetCoordinates();
                if (!coords) {
                    continue;
                }
                const auto cx = static_cast<std::int32_t>(coords->cellX);
                const auto cy = static_cast<std::int32_t>(coords->cellY);
                if (first) {
                    out.minCellX = out.maxCellX = cx;
                    out.minCellY = out.maxCellY = cy;
                    if (auto* ws = cell->GetRuntimeData().worldSpace) {
                        out.worldSpace = ws->GetFormID();
                    }
                    first = false;
                    continue;
                }
                out.minCellX = std::min(out.minCellX, cx);
                out.maxCellX = std::max(out.maxCellX, cx);
                out.minCellY = std::min(out.minCellY, cy);
                out.maxCellY = std::max(out.maxCellY, cy);
            }
        }
        out.valid = !first && out.worldSpace != 0;
        return out;
    }

    Chain Build(const LoadedGrid& grid,
                RE::FormID worldSpace,
                const RE::NiPoint3& visitorOrigin,
                const RE::NiPoint3& playerPos)
    {
        Chain chain;
        const auto assembly = Assemble(grid, worldSpace, visitorOrigin, playerPos);
        if (!assembly.ok) {
            logger::debug("ApproachChain: no worldspace to build in");
            return chain;
        }

        const auto result = AStar(assembly.graph, assembly.visitorNode, assembly.playerNode);
        if (!result.found) {
            logger::warn("ApproachChain: no route from the visitor to the player over {} node(s) -- the "
                         "visitor-to-player line should have made this impossible",
                         assembly.graph.nodes.size());
            return chain;
        }

        std::array<std::size_t, 4> histogram{};
        std::size_t inGrid = 0;
        chain.points.reserve(result.path.size());
        for (const auto index : result.path) {
            const auto& node = assembly.graph.nodes[index];
            Point point;
            point.position = node.pos;
            point.cls = node.cls;
            point.insideLoadedGrid = grid.Contains(worldSpace, node.pos);
            point.fineNode = node.fineNode;
            if (point.insideLoadedGrid) {
                ++inGrid;
            }
            ++histogram[static_cast<std::size_t>(node.cls)];
            chain.points.push_back(point);
        }
        chain.cost = result.cost;
        chain.valid = true;

        logger::debug("ApproachChain: ws={:08X} graph={} nodes/{} edges (coarse={} fine={} of {} network(s)) "
                      "-- chain={} points cost={:.0f} [coarse={} fine={} connector={} direct={}] inGrid={}",
                      worldSpace,
                      assembly.graph.nodes.size(),
                      assembly.graph.EdgeEnds() / 2,
                      assembly.coarseCount,
                      assembly.fineSelected,
                      assembly.fineNetworks,
                      chain.points.size(),
                      chain.cost,
                      histogram[static_cast<std::size_t>(PointClass::Coarse)],
                      histogram[static_cast<std::size_t>(PointClass::Fine)],
                      histogram[static_cast<std::size_t>(PointClass::Connector)],
                      histogram[static_cast<std::size_t>(PointClass::Direct)],
                      inGrid);
        return chain;
    }
} // namespace NarrativeEngine::ApproachChain

namespace NarrativeEngine::ApproachChain_Testing
{
    OracleResult SearchBothWays(const ApproachChain::LoadedGrid& grid,
                                RE::FormID worldSpace,
                                const RE::NiPoint3& visitorOrigin,
                                const RE::NiPoint3& playerPos)
    {
        using namespace NarrativeEngine::ApproachChain;
        OracleResult out;
        const auto assembly = Assemble(grid, worldSpace, visitorOrigin, playerPos);
        if (!assembly.ok) {
            return out;
        }
        out.nodeCount = assembly.graph.nodes.size();
        out.edgeCount = assembly.graph.EdgeEnds() / 2;

        const auto fast = AStar(assembly.graph, assembly.visitorNode, assembly.playerNode);
        const auto slow = Dijkstra(assembly.graph, assembly.visitorNode, assembly.playerNode);
        out.built = fast.found && slow.found;
        out.astarCost = fast.cost;
        out.dijkstraCost = slow.cost;
        for (const auto index : fast.path) {
            out.astarPath.push_back(assembly.graph.nodes[index].pos);
        }
        for (const auto index : slow.path) {
            out.dijkstraPath.push_back(assembly.graph.nodes[index].pos);
        }
        return out;
    }
} // namespace NarrativeEngine::ApproachChain_Testing
