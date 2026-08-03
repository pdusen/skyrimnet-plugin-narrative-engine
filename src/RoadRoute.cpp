#include <RoadRoute.h>

#include <FineRoads.h>
#include <logger.h>
#include <TravelGraph.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace NarrativeEngine::RoadRoute
{
    namespace
    {
        constexpr float kCellUnits = 4096.0f;
        constexpr float kInf = std::numeric_limits<float>::max();

        // How close the destination must be to a fine node before we
        // treat the whole journey as covered by fine data. Two cells is
        // generous on purpose: the alternative is handing off to the
        // coarse graph for a trip that never leaves the loaded region,
        // which would route an NPC out and back for no reason.
        constexpr float kDestinationWithinFineUnits = 2.0f * kCellUnits;

        float Dist2D(float ax, float ay, float bx, float by)
        {
            const float dx = ax - bx;
            const float dy = ay - by;
            return std::sqrt(dx * dx + dy * dy);
        }

        std::size_t NearestFineNode(const FineRoads::Graph& graph, float x, float y)
        {
            std::size_t best = FineRoads::kInvalidNode;
            float bestDistSq = kInf;
            for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
                const float dx = graph.nodes[i].x - x;
                const float dy = graph.nodes[i].y - y;
                const float distSq = dx * dx + dy * dy;
                if (distSq < bestDistSq) {
                    bestDistSq = distSq;
                    best = i;
                }
            }
            return best;
        }

        // Dijkstra over the fine graph. Fills `dist` and `previous`.
        void FineDijkstra(const FineRoads::Graph& graph,
                          std::size_t source,
                          std::vector<float>& dist,
                          std::vector<std::size_t>& previous)
        {
            dist.assign(graph.nodes.size(), kInf);
            previous.assign(graph.nodes.size(), FineRoads::kInvalidNode);
            std::vector<bool> settled(graph.nodes.size(), false);

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
                for (const auto next : graph.adjacency[current]) {
                    if (settled[next]) {
                        continue;
                    }
                    const float step = Dist2D(
                        graph.nodes[next].x, graph.nodes[next].y, graph.nodes[current].x, graph.nodes[current].y);
                    if (d + step < dist[next]) {
                        dist[next] = d + step;
                        previous[next] = current;
                        frontier.emplace(dist[next], next);
                    }
                }
            }
        }

        std::vector<RE::NiPoint3> ReconstructFine(const FineRoads::Graph& graph,
                                                  const std::vector<std::size_t>& previous,
                                                  std::size_t from,
                                                  std::size_t to)
        {
            std::vector<RE::NiPoint3> path;
            for (std::size_t at = to; at != FineRoads::kInvalidNode; at = previous[at]) {
                const auto& n = graph.nodes[at];
                path.push_back(RE::NiPoint3{n.x, n.y, n.z});
                if (at == from) {
                    break;
                }
            }
            std::reverse(path.begin(), path.end());
            return path;
        }

        // Coarse-only plan, used both when there is no fine coverage and
        // as the fallback when the fine graph has no reachable frontier.
        Plan CoarseOnly(RE::FormID worldSpace, const RE::NiPoint3& from, const RE::NiPoint3& to)
        {
            Plan plan;
            const auto start = TravelGraph::FindNearestNode(worldSpace, from.x, from.y);
            const auto dest = TravelGraph::FindNearestNode(worldSpace, to.x, to.y);
            if (start == TravelGraph::kInvalidNode || dest == TravelGraph::kInvalidNode) {
                return plan;
            }
            plan.coarsePath = TravelGraph::FindPath(start, dest);
            if (plan.coarsePath.empty()) {
                return plan;
            }
            plan.valid = true;

            for (std::size_t i = 1; i < plan.coarsePath.size(); ++i) {
                const auto* a = TravelGraph::GetNode(plan.coarsePath[i - 1]);
                const auto* b = TravelGraph::GetNode(plan.coarsePath[i]);
                if (a && b) {
                    plan.estimatedCost += Dist2D(a->x, a->y, b->x, b->y);
                }
            }
            return plan;
        }
    } // namespace

    Origin ResolveOrigin(const MainThread::Token&, RE::TESObjectREFR* ref)
    {
        Origin origin;
        if (!ref) {
            return origin;
        }
        auto* cell = ref->GetParentCell();
        if (!cell) {
            return origin;
        }

        if (!cell->IsInteriorCell()) {
            auto* ws = cell->GetRuntimeData().worldSpace;
            if (!ws) {
                return origin;
            }
            origin.valid = true;
            origin.worldSpace = ws->GetFormID();
            origin.position = ref->GetPosition();
            return origin;
        }

        // Indoors. Walk the cell's references for a load door and take
        // where it comes out. First exterior-landing door wins — cells
        // with several exits are rare, and any of them is a defensible
        // answer to "where would they emerge".
        RE::NiPoint3 arrival{};
        RE::FormID arrivalWorldSpace = 0;
        bool found = false;

        cell->ForEachReference([&](RE::TESObjectREFR* candidate) {
            if (found || !candidate) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            auto* teleport = candidate->extraList.GetByType<RE::ExtraTeleport>();
            if (!teleport || !teleport->teleportData) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            auto linked = teleport->teleportData->linkedDoor.get();
            if (!linked) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            auto* linkedCell = linked->GetParentCell();
            if (!linkedCell || linkedCell->IsInteriorCell()) {
                return RE::BSContainer::ForEachResult::kContinue; // interior-to-interior, keep looking
            }
            auto* ws = linkedCell->GetRuntimeData().worldSpace;
            if (!ws) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            arrivalWorldSpace = ws->GetFormID();
            // teleportData->position is the arrival spot just outside the
            // far door, which is closer to "where they emerge" than the
            // door reference itself.
            arrival = teleport->teleportData->position;
            found = true;
            return RE::BSContainer::ForEachResult::kStop;
        });

        if (found) {
            origin.valid = true;
            origin.worldSpace = arrivalWorldSpace;
            origin.position = arrival;
            origin.viaLoadDoor = true;
            return origin;
        }

        // No load door out. Fall back to the location's map marker, which
        // is coarser (markers sit some way from the actual entrance) but
        // beats having no position at all.
        if (auto* location = ref->GetCurrentLocation()) {
            const auto markerPtr = location->worldLocMarker.get();
            if (auto* marker = markerPtr.get()) {
                if (auto* markerCell = marker->GetParentCell()) {
                    if (auto* ws = markerCell->GetRuntimeData().worldSpace) {
                        origin.valid = true;
                        origin.worldSpace = ws->GetFormID();
                        origin.position = marker->GetPosition();
                        origin.viaLoadDoor = true;
                        return origin;
                    }
                }
            }
        }
        return origin;
    }

    Plan Route(RE::FormID worldSpace, const RE::NiPoint3& from, const RE::NiPoint3& to)
    {
        const auto fine = FineRoads::Snapshot();
        const bool fineUsable = !fine.empty() && fine.worldSpace == worldSpace;
        if (!fineUsable) {
            return CoarseOnly(worldSpace, from, to);
        }

        const auto start = NearestFineNode(fine, from.x, from.y);
        if (start == FineRoads::kInvalidNode) {
            return CoarseOnly(worldSpace, from, to);
        }

        std::vector<float> fineDist;
        std::vector<std::size_t> finePrev;
        FineDijkstra(fine, start, fineDist, finePrev);

        // Destination inside fine coverage: the whole trip is walkable
        // and there is no handoff to make.
        const auto fineDest = NearestFineNode(fine, to.x, to.y);
        if (fineDest != FineRoads::kInvalidNode && fineDist[fineDest] != kInf
            && Dist2D(fine.nodes[fineDest].x, fine.nodes[fineDest].y, to.x, to.y) <= kDestinationWithinFineUnits) {
            Plan plan;
            plan.valid = true;
            plan.destinationWithinFine = true;
            plan.finePath = ReconstructFine(fine, finePrev, start, fineDest);
            plan.estimatedCost = fineDist[fineDest];
            return plan;
        }

        const auto coarseDest = TravelGraph::FindNearestNode(worldSpace, to.x, to.y);
        if (coarseDest == TravelGraph::kInvalidNode) {
            return CoarseOnly(worldSpace, from, to);
        }
        // One Dijkstra over the coarse graph from the destination gives
        // every coarse node's remaining cost, so scoring a frontier is a
        // lookup rather than another search.
        const auto coarseField = TravelGraph::DistanceField(coarseDest);
        if (coarseField.empty()) {
            return CoarseOnly(worldSpace, from, to);
        }

        std::size_t bestFrontier = FineRoads::kInvalidNode;
        std::size_t bestCoarseEntry = TravelGraph::kInvalidNode;
        float bestCost = kInf;

        for (std::size_t i = 0; i < fine.nodes.size(); ++i) {
            if (!fine.nodes[i].frontier || fineDist[i] == kInf) {
                continue;
            }
            const auto& node = fine.nodes[i];
            const auto entry = TravelGraph::FindNearestNode(worldSpace, node.x, node.y);
            if (entry == TravelGraph::kInvalidNode || entry >= coarseField.size()) {
                continue;
            }
            if (coarseField[entry] == kInf) {
                continue; // that part of the network can't reach the destination
            }
            const auto* entryNode = TravelGraph::GetNode(entry);
            if (!entryNode) {
                continue;
            }
            const float join = Dist2D(node.x, node.y, entryNode->x, entryNode->y);
            const float total = fineDist[i] + join + coarseField[entry];
            if (total < bestCost) {
                bestCost = total;
                bestFrontier = i;
                bestCoarseEntry = entry;
            }
        }

        if (bestFrontier == FineRoads::kInvalidNode) {
            // Every fine branch dead-ends inside the loaded region — the
            // player is at the end of a spur, or the road data here is
            // isolated. A fine path here would walk the NPC into a dead
            // end, so hand back a coarse-only plan instead.
            logger::debug("RoadRoute: no reachable frontier among {} fine node(s); falling back to coarse-only",
                          fine.nodes.size());
            return CoarseOnly(worldSpace, from, to);
        }

        Plan plan;
        plan.valid = true;
        plan.finePath = ReconstructFine(fine, finePrev, start, bestFrontier);
        plan.coarsePath = TravelGraph::FindPath(bestCoarseEntry, coarseDest);
        plan.estimatedCost = bestCost;
        return plan;
    }
} // namespace NarrativeEngine::RoadRoute
