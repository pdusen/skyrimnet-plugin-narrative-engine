#include <RoadRoute.h>

#include <FineRoads.h>
#include <logger.h>
#include <TravelGraph.h>
#include <VisitorTravelLog.h>

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

        // How much lower one door's landing has to be than another's before
        // elevation decides instead of distance. One storey, measured: the
        // Bannered Mare's balcony landing sits 194 units above its front
        // door's.
        constexpr float kDoorStoreyUnits = 128.0f;
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

    RE::TESObjectCELL* CellOf(RE::TESObjectREFR* ref)
    {
        if (!ref) {
            return nullptr;
        }
        if (auto* attached = ref->GetParentCell()) {
            return attached;
        }
        return ref->GetSaveParentCell();
    }

    namespace
    {
        // Upper bound on parentLoc traversal, matching AlphaCanon's. A
        // room's Location rarely carries a map marker; the building or
        // the settlement above it does.
        constexpr int kMaxParentDepth = 16;
    } // namespace

    bool MarkerFromLocation(RE::BGSLocation* location, Origin& out, std::string& trail)
    {
        for (int depth = 0; location && depth < kMaxParentDepth; ++depth) {
            const char* edid = location->GetFormEditorID();
            trail += (depth == 0 ? "" : "->");
            trail += (edid && *edid) ? edid : "?";

            const auto markerPtr = location->worldLocMarker.get();
            if (auto* marker = markerPtr.get()) {
                // A marker away from the player is in an unloaded cell,
                // which is exactly when this matters -- so CellOf.
                if (auto* markerCell = CellOf(marker)) {
                    if (auto* ws = markerCell->GetRuntimeData().worldSpace) {
                        out.valid = true;
                        out.worldSpace = ws->GetFormID();
                        out.position = marker->GetPosition();
                        out.viaLoadDoor = true;
                        trail += "[marker]";
                        return true;
                    }
                    trail += "[marker,no-worldspace]";
                } else {
                    trail += "[marker,no-cell]";
                }
            }
            location = location->parentLoc;
        }
        return false;
    }

    namespace
    {
        // Minimal Use is the engine's own "NPCs should not route through
        // this door" — Bethesda sets it on back doors, service entrances
        // and the like. WRDragonDoor01MinUse (0x08648C) carries it and
        // WRDragonDoor01 (0x0252C7) does not, and those two are exactly
        // the pair in WhiterunBanneredMare.
        bool IsMinimalUseDoor(RE::TESObjectREFR* ref)
        {
            if (!ref) {
                return false;
            }
            auto* base = ref->GetBaseObject();
            auto* door = base ? base->As<RE::TESObjectDOOR>() : nullptr;
            return door && door->flags.any(RE::TESObjectDOOR::Flag::kMinimalUse);
        }
    } // namespace

    Origin ResolveOrigin(const MainThread::Token&, RE::TESObjectREFR* ref)
    {
        Origin origin;
        if (!ref) {
            return origin;
        }
        auto* cell = CellOf(ref);
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

        // Indoors. Collect every load door that lands outdoors, then
        // choose between them.
        //
        // This used to stop at the first one, on the grounds that "cells
        // with several exits are rare, and any of them is a defensible
        // answer". Both halves are false. 279 of the 544 vanilla
        // interiors that have a load door have more than one, and which
        // one `ForEachReference` reaches first is not stable: two
        // sessions a half-hour apart on the same build resolved
        // WhiterunBanneredMare to its front door once and to its back
        // door once. A visitor waiting at the back of an inn the player
        // walked into the front of is not a defensible answer.
        struct Doorway
        {
            RE::FormID worldSpace = 0;
            RE::FormID farDoor = 0;
            RE::NiPoint3 arrival{};
            float fromRef = 0.0f;
            bool minimalUse = false;
        };
        std::vector<Doorway> doorways;

        cell->ForEachReference([&](RE::TESObjectREFR* candidate) {
            if (!candidate) {
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
            // The whole reason for this walk is that `ref` is indoors,
            // and standing indoors is what unloads the exterior. So the
            // door on the far side is precisely the reference whose
            // parentCell is null, and reading it directly made every
            // indoors resolution fail.
            auto* linkedCell = CellOf(linked.get());
            if (!linkedCell || linkedCell->IsInteriorCell()) {
                return RE::BSContainer::ForEachResult::kContinue; // interior-to-interior, keep looking
            }
            auto* ws = linkedCell->GetRuntimeData().worldSpace;
            if (!ws) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            Doorway way;
            way.worldSpace = ws->GetFormID();
            way.farDoor = linked->GetFormID();
            // teleportData->position is the arrival spot just outside the
            // far door, which is closer to "where they emerge" than the
            // door reference itself.
            way.arrival = teleport->teleportData->position;
            // Measured between the near side and the occupant, both of
            // which are in this interior's own frame. The far side's
            // coordinates are the exterior's and would mean nothing here.
            const auto nearSide = candidate->GetPosition();
            const auto occupant = ref->GetPosition();
            way.fromRef = Dist2D(nearSide.x, nearSide.y, occupant.x, occupant.y);
            way.minimalUse = IsMinimalUseDoor(candidate);
            doorways.push_back(way);
            return RE::BSContainer::ForEachResult::kContinue;
        });

        // A door the engine does not mark Minimal Use first; then the one at
        // street level; then the one the occupant is standing nearest. In a
        // one-door cell it is the only door, so the common case is untouched.
        //
        // Minimal Use is a last resort rather than a disqualification: a back
        // door still beats falling through to a map marker.
        //
        // Elevation comes before distance because nearest was the right answer
        // to the wrong question. WhiterunBanneredMare has three ways out, and
        // with the player standing UPSTAIRS the nearest was a mod-added
        // balcony door at 942 units, not flagged Minimal Use, whose landing
        // sits 194 units above the front door's. The balcony genuinely was
        // nearest. It is also somewhere a visitor cannot be walked to, and
        // somewhere nobody leaves a building by: a main entrance is at street
        // level and the upper doors are balconies.
        //
        // kDoorStoreyUnits is below the 194 the measurement shows and well
        // above the jitter between two doors on one floor, so two street-level
        // doors are still settled by distance.
        const Doorway* chosen = nullptr;
        for (const auto& way : doorways) {
            if (chosen == nullptr) {
                chosen = &way;
                continue;
            }
            if (chosen->minimalUse != way.minimalUse) {
                if (!way.minimalUse) {
                    chosen = &way;
                }
                continue;
            }
            const float drop = chosen->arrival.z - way.arrival.z;
            if (drop > kDoorStoreyUnits) {
                chosen = &way; // a storey lower: street level against a balcony
                continue;
            }
            if (drop < -kDoorStoreyUnits) {
                continue; // the one already held is the lower
            }
            if (way.fromRef < chosen->fromRef) {
                chosen = &way;
            }
        }

        if (VisitorTravelLog::IsActive() && doorways.size() > 1) {
            for (const auto& way : doorways) {
                VisitorTravelLog::Write("ENDS",
                                        "  way out: door 0x{:08X} lands at ({:.0f},{:.0f},{:.0f}), {:.0f}u from "
                                        "the occupant, minimal_use={}{}",
                                        way.farDoor,
                                        way.arrival.x,
                                        way.arrival.y,
                                        way.arrival.z,
                                        way.fromRef,
                                        way.minimalUse ? 1 : 0,
                                        (chosen == &way) ? " <- taken" : "");
            }
        }

        if (chosen != nullptr) {
            if (doorways.size() > 1) {
                logger::debug("RoadRoute: cell 0x{:08X} has {} way(s) out; took door 0x{:08X} {:.0f}u from "
                              "the occupant (minimal_use={})",
                              cell->GetFormID(),
                              doorways.size(),
                              chosen->farDoor,
                              chosen->fromRef,
                              chosen->minimalUse);
            }
            origin.valid = true;
            origin.worldSpace = chosen->worldSpace;
            origin.position = chosen->arrival;
            origin.viaLoadDoor = true;
            origin.exteriorDoor = chosen->farDoor;
            return origin;
        }

        // No load door out. Fall back to a map marker, which is coarser
        // -- markers sit some way from the actual entrance -- but beats
        // having no position at all. The cell's own Location first,
        // because that is the building; `GetCurrentLocation` can be null
        // in a cell nobody has registered.
        std::string trail;
        if (MarkerFromLocation(cell->GetLocation(), origin, trail)) {
            logger::debug("RoadRoute: 0x{:08X} placed from its cell's location ({})", ref->GetFormID(), trail);
            return origin;
        }
        if (MarkerFromLocation(ref->GetCurrentLocation(), origin, trail)) {
            logger::debug("RoadRoute: 0x{:08X} placed from its current location ({})", ref->GetFormID(), trail);
            return origin;
        }

        logger::debug("RoadRoute: 0x{:08X} is indoors with no way out — no load door to an exterior and no "
                      "map marker up the location chain ('{}')",
                      ref->GetFormID(),
                      trail);
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
