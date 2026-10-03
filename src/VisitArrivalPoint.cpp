#include <VisitArrivalPoint.h>

#include <ApproachChain.h>
#include <CameraVisibility.h>
#include <FineRoads.h>
#include <logger.h>
#include <MainThread.h>
#include <RoadRoute.h>
#include <Settings.h>
#include <StuckRecovery.h>
#include <TravelGraph.h>

#include <RE/B/BGSLocation.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESWorldSpace.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>

namespace NarrativeEngine::VisitArrivalPoint
{
    namespace
    {
        using StuckRecovery::kGroundClearanceUnits;

        // Body height handed to the cover gate. NOT the nominal 128 that
        // StuckRecovery uses for a humanoid, and deliberately taller than
        // any visitor.
        //
        // `IsPositionBehindCover` samples three heights -- 10%, 50% and
        // 90% of whatever it is given -- so the topmost ray lands at 0.9h
        // above the feet, not at h. Passing the nominal 128 put the top
        // ray at 115 while a visitor's crown sits near 128, and an Altmer's
        // nearer 138. Cover that stops all three rays can therefore leave a
        // head in plain view, which is what a tester watched happen.
        //
        // 160 puts the top ray at 144, clear of the tallest playable race
        // with margin. Deliberately conservative: the cost of being too
        // tall is a spot rejected that would have been fine, and the cost
        // of being too short is the player watching somebody appear.
        constexpr float kCoverProbeHeightUnits = 160.0f;

        // Runner-ups kept for StuckRecovery, and how far apart two of
        // them have to be to count as distinct options.
        //
        // The fine graph is a ribbon two or three nodes wide rather than
        // a thinned centreline, so consecutive nodes can be a hundred
        // units apart ACROSS the road rather than along it. Two points
        // that close are one place, and handing both to the escort
        // spends two escalations on the same patch of ground.
        constexpr std::size_t kMaxFallbacks = 6;
        constexpr float kFallbackSeparationUnits = 400.0f;

        // The city approach's arc around the gate, and how finely it is
        // sampled.
        //
        // The bearing is only as good as the coarse node it points at,
        // and those sit roughly one cell apart — so treating it as exact
        // would be false precision. Thirty degrees either side is wide
        // enough to find usable ground without the arrival reading as
        // coming from somewhere else.
        constexpr float kBearingArcHalfWidthDegrees = 30.0f;
        constexpr int kBearingAzimuthSamples = 9;
        constexpr int kBearingRadiusSamples = 5;

        float Dist2D(const RE::NiPoint3& a, const RE::NiPoint3& b)
        {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            return std::sqrt(dx * dx + dy * dy);
        }

        // A coarse node nearer than this to the player says nothing
        // about direction — CoarseOnly plans start at the node NEAREST
        // the player, which can be almost on top of them. Walk the
        // coarse path until it has actually gone somewhere.
        constexpr float kBearingMinSeparationUnits = 1000.0f;

        // Largest elevation difference from the player a city-approach
        // candidate may have, either way.
        //
        // A reachability proxy, not an aesthetic one: the navmesh gate
        // answers containment and not connectivity, so a ledge is "on
        // navmesh" with no walkable route down. Absolute rather than a
        // slope ratio, so the budget tightens with range: across the
        // 800-5000 band it works out to 26 degrees at the near end and
        // under 5 at the far one. That asymmetry is wanted — the far
        // samples are the ones most likely to reach across a valley onto
        // ground nothing can walk to.
        constexpr float kMaxElevationDeltaUnits = 400.0f;

        // How far out the search may reach when nothing inside the band
        // works.
        //
        // The band's ceiling is a preference about how long the walk in
        // should take, not a limit on where somebody may stand. When no
        // point inside it is hidden, a point beyond it is better than no
        // visit -- and at that range the arrival is not something the
        // player could pick out anyway.
        //
        // Capped near the loaded cell grid (uGridsToLoad 5 gives roughly
        // 8192 units): past that an actor has no 3D at all, so there is
        // nothing to place and nothing to hide.
        constexpr float kMaxReachUnits = 8000.0f;

        // A point this far away, with the player facing elsewhere, may be
        // used even with no cover at all — but only once nothing with
        // real cover has been found. See `Acceptance`.
        //
        // Cover exists to hide the instant somebody appears. If the
        // player is not looking that way, that instant is already
        // hidden, and insisting on geometry as well turns "no visit" into
        // the common outcome on open ground. Players are not watching the
        // direction a visitor will come from most of the time, and this
        // is the concession that takes advantage of it.
        //
        // Far enough out that a figure appearing is small and ambiguous
        // even if the player turns a moment later. Deliberately well
        // above the arrival floor, so this is a fallback for open ground
        // rather than a second way of satisfying the ordinary case.
        constexpr float kUnseenDistanceUnits = 5000.0f;

        // Half-angle of the arc treated as "the player can see this".
        //
        // Deliberately wider than Skyrim's default field of view (~80
        // degrees total), because being wrong in this direction costs a
        // usable spot while being wrong the other way costs the player
        // watching somebody appear. A small turn of the head should not
        // reveal a spawn that was judged unseen.
        constexpr float kViewHalfAngleDegrees = 60.0f;

        // True when `pos` lies outside the arc the player is facing.
        //
        // Uses the PLAYER's facing rather than the camera's: `angle.z` is
        // measured clockwise from +Y, a convention this project already
        // relies on elsewhere, whereas the camera node's rotation matrix
        // convention is not established here and is not worth guessing
        // at. In first person the two are the same. In third person with
        // free-look they can differ, which is why the arc is generous.
        bool OutsidePlayerView(const RE::NiPoint3& pos, const RE::NiPoint3& playerPos, float playerAngleZ)
        {
            const float toX = pos.x - playerPos.x;
            const float toY = pos.y - playerPos.y;
            if (std::fabs(toX) < 1e-3f && std::fabs(toY) < 1e-3f) {
                return false;
            }
            const float facingX = std::sin(playerAngleZ);
            const float facingY = std::cos(playerAngleZ);
            const float len = std::sqrt(toX * toX + toY * toY);
            const float dot = (toX * facingX + toY * facingY) / len;
            const float limit = std::cos(kViewHalfAngleDegrees * 3.14159265f / 180.0f);
            return dot < limit;
        }

        float ToDegrees(float radians)
        {
            return radians * 180.0f / 3.14159265f;
        }

        // Why a candidate was rejected, so a failed search names the
        // gate that killed it rather than going quiet.
        struct GateTally
        {
            int considered = 0;
            int tooNear = 0;
            // Past the furthest the search may reach at all, not merely
            // past the preferred band -- the band's ceiling now only
            // decides which pool a candidate lands in.
            int tooFar = 0;
            int offNavmesh = 0;
            int notLevel = 0;
            int inView = 0;
            // Nothing walkable joining the candidate to the player along
            // the straight line. Counted apart from the rest because it
            // is the only gate about REACHING the point rather than
            // standing on it, and a run where it dominates is telling
            // you the terrain is cut up rather than exposed.
            int noCorridor = 0;
            // Accepted with no cover because the player was facing the
            // other way. Counted apart from the covered ones: a run that
            // leans on this is telling you the terrain had nothing to
            // hide behind, which is the finding, not an incidental.
            int unseen = 0;

            // How far out the candidates actually reached, whether or not
            // the band admitted them.
            //
            // The counts alone cannot distinguish "the band is throwing
            // away road that would have worked" from "there is no cover
            // out there either" -- and those want opposite responses. The
            // span says how much road exists past the ceiling, so raising
            // it can be judged before it is tried rather than after.
            float nearest = -1.0f;
            float farthest = -1.0f;

            void Saw(float distance)
            {
                ++considered;
                if (nearest < 0.0f || distance < nearest) {
                    nearest = distance;
                }
                if (distance > farthest) {
                    farthest = distance;
                }
            }

            std::string Describe() const
            {
                char span[64] = "span=none";
                if (nearest >= 0.0f) {
                    std::snprintf(span, sizeof(span), "span=[%.0f,%.0f]", nearest, farthest);
                }
                return "considered=" + std::to_string(considered) + " " + span + " tooNear=" + std::to_string(tooNear)
                       + " pastReach=" + std::to_string(tooFar) + " offNavmesh=" + std::to_string(offNavmesh)
                       + " notLevel=" + std::to_string(notLevel) + " inView=" + std::to_string(inView) + " noCorridor="
                       + std::to_string(noCorridor) + " unseen=" + std::to_string(unseen) + " survived="
                       + std::to_string(considered - tooNear - offNavmesh - notLevel - inView - noCorridor);
            }
        };

        // Accepted candidates, best-first. The winner is element zero;
        // everything after it is fallback supply.
        void AppendSeparated(std::vector<RE::NiPoint3>& kept, const RE::NiPoint3& candidate)
        {
            for (const auto& existing : kept) {
                if (Dist2D(existing, candidate) < kFallbackSeparationUnits) {
                    return;
                }
            }
            kept.push_back(candidate);
        }

        bool BehindCover(const RE::NiPoint3& pos, float coverRadius)
        {
            return CameraVisibility::IsPositionBehindCover(
                pos,
                kCoverProbeHeightUnits,
                coverRadius,
                static_cast<float>(std::max(0, Settings::Get().visitArrivalCoverProximityUnits)));
        }

        // ---- The chain walk ----------------------------------------
        //
        // One candidate the arrival search may consider.
        //
        // Rank is how safe the point is to stand on, which is the reverse
        // of how cheap it was to route through: real road the engine has
        // loaded first, unvalidated ground outside the grid last. It
        // decides between candidates AT THE SAME DISTANCE — which is
        // exactly what hop expansion produces, a handful of points metres
        // apart — and never overrides the outward walk itself.
        struct Candidate
        {
            RE::NiPoint3 standing{};
            ApproachChain::PointClass cls = ApproachChain::PointClass::Coarse;
            bool insideGrid = false;
            int rank = 5;
            int hopCount = 0;
            // Position along the chain, visitor-first. Fallbacks are
            // ordered by this and not by distance, because the escort
            // walks a stalled visitor BACK ALONG THEIR OWN PATH.
            std::size_t chainIndex = 0;
            float distance = 0.0f;
            // Filled by the walk, not by the gatherer.
            int grade = 0;
        };

        int RankOf(ApproachChain::PointClass cls, bool insideGrid, int hopCount)
        {
            using PC = ApproachChain::PointClass;
            if (cls == PC::Fine) {
                return hopCount == 0 ? 1 : 2;
            }
            if (cls == PC::Coarse) {
                return 5;
            }
            // Connector and Direct are both synthetic. What decides their
            // rank is WHERE THEY ARE, not which line produced them: a
            // synthetic point inside the loaded grid can be navmesh
            // checked and cover tested like any fine node, and should be,
            // because a validated off-road spot beats an unvalidated one
            // every time. Gating by segment instead of by position would
            // throw away the one part of each synthetic run that can
            // actually be verified.
            return insideGrid ? 3 : 4;
        }

        // Every point the search may consider: the chain itself, plus the
        // fine nodes within `hopRadius` hops of a fine point on it.
        //
        // Hop expansion earns its place because the fine graph is a road
        // RIBBON — a 1-hop neighbour is usually the other side of the same
        // road, and a 2-hop neighbour is a few metres off it. That is
        // where cheap cover lives when the road itself is in plain view,
        // and it converts a declined visit into a placed one without
        // moving the visitor anywhere the player would find strange.
        //
        // Hops are undirected, so an expanded node can sit back toward the
        // player. The minimum-distance gate in the walk is what rules
        // those out: a node on the far side of the player is at most one
        // two-hop reach away from them, and that reach is a measured
        // 1,784 units at worst across all of vanilla, median 226 (see
        // docs/engine-findings/fine-road-hop-spans.md). At a 2,000-unit
        // floor every one of them is inside it and rejected on distance
        // before direction is ever a question, which is why neither the
        // Phase 14 approach corridor nor any other directional test is
        // carried over — they would never change an answer.
        std::vector<Candidate> GatherCandidates(const ApproachChain::Chain& chain,
                                                const FineRoads::Graph& fine,
                                                int hopRadius,
                                                const RE::NiPoint3& anchorPos)
        {
            std::vector<Candidate> out;
            out.reserve(chain.points.size() * 2);

            // Cheapest hop count found for each fine node, so a node
            // reachable from two chain points is kept once at its best
            // standing rather than twice.
            std::unordered_map<std::size_t, int> hopsByNode;
            for (std::size_t i = 0; i < chain.points.size(); ++i) {
                const auto& point = chain.points[i];
                Candidate candidate;
                candidate.standing = point.position;
                candidate.standing.z += kGroundClearanceUnits;
                candidate.cls = point.cls;
                candidate.insideGrid = point.insideLoadedGrid;
                candidate.hopCount = 0;
                candidate.rank = RankOf(point.cls, point.insideLoadedGrid, 0);
                candidate.chainIndex = i;
                candidate.distance = Dist2D(point.position, anchorPos);
                out.push_back(candidate);
                if (point.fineNode != FineRoads::kInvalidNode) {
                    hopsByNode.emplace(point.fineNode, 0);
                }
            }

            if (hopRadius <= 0 || fine.nodes.empty()) {
                return out;
            }

            // Breadth-first off every fine node the chain touches, which
            // `FineRoads::Graph::adjacency` already answers — no new graph
            // work, and bounded by the radius rather than by the graph.
            std::vector<std::pair<std::size_t, int>> frontier;
            for (const auto& entry : hopsByNode) {
                frontier.emplace_back(entry.first, entry.second);
            }
            while (!frontier.empty()) {
                std::vector<std::pair<std::size_t, int>> next;
                for (const auto& entry : frontier) {
                    const auto node = entry.first;
                    const auto hops = entry.second;
                    if (hops >= hopRadius || node >= fine.adjacency.size()) {
                        continue;
                    }
                    for (const auto neighbor : fine.adjacency[node]) {
                        if (neighbor >= fine.nodes.size()) {
                            continue;
                        }
                        const auto existing = hopsByNode.find(neighbor);
                        if (existing != hopsByNode.end() && existing->second <= hops + 1) {
                            continue;
                        }
                        hopsByNode[neighbor] = hops + 1;
                        next.emplace_back(neighbor, hops + 1);
                    }
                }
                frontier = std::move(next);
            }

            // Which chain point each expanded node hangs off, so its
            // fallback ordering stays the chain's rather than becoming
            // arbitrary.
            for (const auto& entry : hopsByNode) {
                if (entry.second == 0) {
                    continue; // already in, as an on-chain point
                }
                const auto& n = fine.nodes[entry.first];
                Candidate candidate;
                candidate.standing = RE::NiPoint3{n.x, n.y, n.z};
                candidate.standing.z += kGroundClearanceUnits;
                candidate.cls = ApproachChain::PointClass::Fine;
                candidate.insideGrid = true; // fine nodes come from attached cells
                candidate.hopCount = entry.second;
                candidate.rank = RankOf(ApproachChain::PointClass::Fine, true, entry.second);
                candidate.distance = Dist2D(candidate.standing, anchorPos);
                std::size_t nearestChain = 0;
                float nearestDist = -1.0f;
                for (std::size_t i = 0; i < chain.points.size(); ++i) {
                    if (chain.points[i].fineNode == FineRoads::kInvalidNode) {
                        continue;
                    }
                    const float d = Dist2D(chain.points[i].position, candidate.standing);
                    if (nearestDist < 0.0f || d < nearestDist) {
                        nearestDist = d;
                        nearestChain = i;
                    }
                }
                candidate.chainIndex = nearestChain;
                out.push_back(candidate);
            }
            return out;
        }

        // Does this candidate's class let it be stood on at all?
        //
        // Two questions for anything inside the loaded grid, and neither
        // is the retired approach corridor: `IsOnNavmesh` says the point
        // is walkable ground, and `HasNavmeshCorridor` says the visitor
        // can walk FROM it TO the player. The second was added in Phase 14
        // against a real failure — a visitor halting at a dead end 550
        // units below the player — and being on navmesh does not imply it.
        //
        // Outside the grid neither is answerable: there is no navmesh to
        // read and no geometry for a ray to hit. Such a point is accepted
        // on distance alone, which is consistent rather than lax, because
        // beyond view distance cover is moot.
        bool PassesClassGates(const Candidate& candidate, const RE::NiPoint3& anchorPos, GateTally& tally)
        {
            if (!candidate.insideGrid) {
                return true;
            }
            if (!StuckRecovery::IsOnNavmesh(candidate.standing)) {
                ++tally.offNavmesh;
                return false;
            }
            if (!StuckRecovery::HasNavmeshCorridor(candidate.standing, anchorPos)) {
                ++tally.noCorridor;
                return false;
            }
            return true;
        }

        // How hidden a candidate is, best first.
        //
        // These are PREFERENCES, not alternatives. The whole of the
        // better grade is considered before any of the worse one, so a
        // point with real cover always beats an uncovered one — even when
        // the uncovered one is nearer and the outward walk reaches it
        // first.
        enum class Acceptance : std::uint8_t
        {
            // Real geometry between the player and the arrival. The
            // instant somebody appears is hidden whatever the player is
            // doing.
            Cover = 0,
            // No cover, but far enough out AND outside the arc the player
            // is facing. Players are not watching the direction a visitor
            // comes from most of the time, and on open ground this is the
            // difference between a visit and no visit — but it depends on
            // where the player happens to be looking, so it is a
            // fallback rather than an equal.
            Unseen = 1,
            // Outside the loaded grid, where neither question is
            // answerable: no navmesh to read, no geometry for a ray to
            // hit. Accepted on distance alone, which is consistent rather
            // than lax, because out there cover is moot — and last,
            // because a verified spot beats an unverifiable one.
            Unverifiable = 2,
            Rejected = 3,
        };

        const char* AcceptanceName(Acceptance acceptance)
        {
            switch (acceptance) {
            case Acceptance::Cover:
                return "cover";
            case Acceptance::Unseen:
                return "unseen";
            case Acceptance::Unverifiable:
                return "outside-grid";
            case Acceptance::Rejected:
            default:
                return "rejected";
            }
        }

        // Graded once per candidate, because the cover test is a raycast
        // and grading per preference pass would pay for it twice.
        Acceptance GradeCandidate(const Candidate& candidate,
                                  const RE::NiPoint3& anchorPos,
                                  float playerAngleZ,
                                  float coverRadius,
                                  GateTally& tally)
        {
            if (!candidate.insideGrid) {
                return Acceptance::Unverifiable;
            }
            if (BehindCover(candidate.standing, coverRadius)) {
                return Acceptance::Cover;
            }
            if (candidate.distance >= kUnseenDistanceUnits
                && OutsidePlayerView(candidate.standing, anchorPos, playerAngleZ)) {
                ++tally.unseen;
                return Acceptance::Unseen;
            }
            ++tally.inView;
            return Acceptance::Rejected;
        }

        // One walk outward from the player, over every candidate.
        //
        // Replaces the band and the two road tiers under it. The band's
        // ceiling was always a preference rather than a rule — a point
        // 6,000 units out beat no visit — and once the chain supplies
        // candidates the whole way home, the simpler rule gives better
        // answers: take the FIRST point far enough away that is hidden.
        //
        // The design's second step, "if nothing is obscured, take the
        // first point beyond the player's view", needs no second pass.
        // Outward IS the order, and a point outside the loaded grid is
        // beyond any view the player has of real geometry — so once every
        // point inside the grid has been refused, the walk arrives at the
        // bridge, coarse and direct-line points on its own. That is what
        // they are for.
        std::vector<Candidate> WalkChain(const std::vector<Candidate>& candidates,
                                         const RE::NiPoint3& anchorPos,
                                         float playerAngleZ,
                                         float minDist,
                                         float coverRadius,
                                         bool allowOutsideGrid,
                                         GateTally& tally)
        {
            std::vector<Candidate> eligible;
            for (const auto& candidate : candidates) {
                tally.Saw(candidate.distance);
                if (candidate.distance < minDist) {
                    ++tally.tooNear;
                    continue;
                }
                if (!candidate.insideGrid && !allowOutsideGrid) {
                    // The stricter reading of "visitors arrive from
                    // somewhere real": confine every arrival to ground
                    // the engine has attached, and decline rather than
                    // reach past it.
                    ++tally.tooFar;
                    continue;
                }
                if (!PassesClassGates(candidate, anchorPos, tally)) {
                    continue;
                }
                eligible.push_back(candidate);
            }

            // Outward from the player, with rank and then hop count
            // breaking ties between candidates at the same distance.
            std::sort(eligible.begin(), eligible.end(), [](const Candidate& a, const Candidate& b) {
                if (a.distance != b.distance) {
                    return a.distance < b.distance;
                }
                if (a.rank != b.rank) {
                    return a.rank < b.rank;
                }
                return a.hopCount < b.hopCount;
            });

            // Grade what survived, and find the best grade anything
            // reached. Cover is preferred outright: the whole of it is
            // considered before any uncovered point, however near.
            auto best = Acceptance::Rejected;
            std::vector<Candidate> graded;
            for (auto& candidate : eligible) {
                const auto grade = GradeCandidate(candidate, anchorPos, playerAngleZ, coverRadius, tally);
                if (grade == Acceptance::Rejected) {
                    continue;
                }
                candidate.grade = static_cast<int>(grade);
                if (grade < best) {
                    best = grade;
                }
                graded.push_back(candidate);
            }
            if (graded.empty()) {
                return {};
            }

            // The winner comes from the best grade, nearest first, because
            // `graded` is still in outward order. Fallbacks may come from
            // any grade: they are only ever reached when the visitor is
            // already stuck, every grade here is one the player is not
            // watching, and starving the escort to keep the ladder tidy
            // helps nobody.
            std::vector<Candidate> kept;
            for (const bool winnersOnly : {true, false}) {
                for (const auto& candidate : graded) {
                    const bool isBest = candidate.grade == static_cast<int>(best);
                    if (winnersOnly != isBest) {
                        continue;
                    }
                    bool tooClose = false;
                    for (const auto& existing : kept) {
                        if (Dist2D(existing.standing, candidate.standing) < kFallbackSeparationUnits) {
                            tooClose = true;
                            break;
                        }
                    }
                    if (tooClose) {
                        continue;
                    }
                    kept.push_back(candidate);
                    logger::debug("VisitArrivalPoint: candidate ({:.0f},{:.0f},{:.0f}) kept — {:.0f}u out, "
                                  "{:+.0f}u in elevation, class={} rank={} hops={} in_grid={}, passed by {}",
                                  candidate.standing.x,
                                  candidate.standing.y,
                                  candidate.standing.z,
                                  candidate.distance,
                                  candidate.standing.z - anchorPos.z,
                                  ApproachChain::PointClassName(candidate.cls),
                                  candidate.rank,
                                  candidate.hopCount,
                                  candidate.insideGrid,
                                  AcceptanceName(static_cast<Acceptance>(candidate.grade)));
                    if (kept.size() > kMaxFallbacks) {
                        break;
                    }
                }
                if (kept.size() > kMaxFallbacks) {
                    break;
                }
            }

            // The rest are fallback supply and go back into CHAIN order,
            // outward of the winner, because that is what the escort
            // promises to walk back along.
            std::vector<Candidate> ordered;
            ordered.push_back(kept.front());
            std::vector<Candidate> rest(kept.begin() + 1, kept.end());
            std::sort(rest.begin(), rest.end(), [](const Candidate& a, const Candidate& b) {
                if (a.chainIndex != b.chainIndex) {
                    return a.chainIndex > b.chainIndex;
                }
                return a.distance < b.distance;
            });
            for (const auto& candidate : rest) {
                ordered.push_back(candidate);
            }
            return ordered;
        }

        // ---- The city approach's candidate generator ---------------
        //
        // Phase 16 took the road path off this: the approach chain has real
        // candidates everywhere and no bearing to aim. Tier::CityApproach
        // still places a visitor between the player and the city gate by
        // sampling an arc, and that tier is deliberately out of scope for
        // this phase, so the primitive stays with one caller instead of two.

        struct BearingCandidate
        {
            RE::NiPoint3 pos{};
            // Angular deviation from the bearing home, in degrees, and
            // distance from the middle of the band. Combined so that
            // neither is a pure tiebreaker for the other: a point
            // straight down the bearing at the edge of the band should
            // not automatically beat one slightly off it at a better
            // distance.
            float score = 0.0f;
        };

        std::vector<RE::NiPoint3> SampleBearingArc(const RE::NiPoint3& playerPos,
                                                   float playerAngleZ,
                                                   const RE::NiPoint3& toward,
                                                   float minDist,
                                                   float maxDist,
                                                   float coverRadius,
                                                   GateTally& tally)
        {
            const float baseAngle = std::atan2(toward.y - playerPos.y, toward.x - playerPos.x);
            const float arc = kBearingArcHalfWidthDegrees * 3.14159265f / 180.0f;

            std::vector<BearingCandidate> accepted;
            const float bandMiddle = 0.5f * (minDist + maxDist);
            const float bandHalf = std::max(1.0f, 0.5f * (maxDist - minDist));

            for (int r = 0; r < kBearingRadiusSamples; ++r) {
                const float t = kBearingRadiusSamples == 1 ? 0.0f : static_cast<float>(r) / (kBearingRadiusSamples - 1);
                // Spread across the whole reach, not just the band:
                // the far rings are only ever reached when the near ones
                // failed, and the scoring below already prefers the
                // middle of the band over them.
                const float outer = std::max(maxDist, kMaxReachUnits);
                const float radius = minDist + t * (outer - minDist);
                for (int a = 0; a < kBearingAzimuthSamples; ++a) {
                    const float u =
                        kBearingAzimuthSamples == 1 ? 0.0f : static_cast<float>(a) / (kBearingAzimuthSamples - 1);
                    const float offset = -arc + u * (2.0f * arc);
                    const float angle = baseAngle + offset;

                    tally.Saw(radius);
                    RE::NiPoint3 probe{
                        playerPos.x + radius * std::cos(angle), playerPos.y + radius * std::sin(angle), playerPos.z};

                    // Off-road ground carries none of Tier 1's
                    // guarantees, so this is the full standability
                    // question: grounded, dry, and on navmesh.
                    RE::NiPoint3 standing{};
                    if (!StuckRecovery::IsStandable(probe, standing)) {
                        ++tally.offNavmesh;
                        continue;
                    }
                    if (std::fabs(standing.z - playerPos.z) > kMaxElevationDeltaUnits) {
                        ++tally.notLevel;
                        continue;
                    }
                    if (!BehindCover(standing, coverRadius)) {
                        if (radius < kUnseenDistanceUnits || !OutsidePlayerView(standing, playerPos, playerAngleZ)) {
                            ++tally.inView;
                            continue;
                        }
                        ++tally.unseen;
                    }

                    if (!StuckRecovery::HasNavmeshCorridor(standing, playerPos)) {
                        ++tally.noCorridor;
                        continue;
                    }

                    BearingCandidate candidate;
                    candidate.pos = standing;
                    const float angularPenalty = std::fabs(offset) / arc;
                    const float distancePenalty = std::fabs(radius - bandMiddle) / bandHalf;
                    candidate.score = angularPenalty + distancePenalty;
                    accepted.push_back(candidate);
                }
            }

            std::sort(accepted.begin(), accepted.end(), [](const BearingCandidate& a, const BearingCandidate& b) {
                return a.score < b.score;
            });

            std::vector<RE::NiPoint3> kept;
            for (const auto& candidate : accepted) {
                AppendSeparated(kept, candidate.pos);
                if (kept.size() > kMaxFallbacks) {
                    break;
                }
            }
            return kept;
        }

        // How a visitor's end of the route was found. Logged, because
        // which rung answered says how much to trust the direction: a
        // live position is where they actually are, a map marker is only
        // roughly where they belong.
        enum class OriginSource : std::uint8_t
        {
            Unresolved,
            Live,          // RoadRoute::ResolveOrigin -- loaded, or indoors via a load door
            SaveCell,      // unloaded, and the save files them in an exterior cell
            SaveCellLocal, // unloaded in an interior; the map marker of that cell's Location
            HomeLocation   // not even that; the marker of the Location the record files them under
        };

        const char* OriginSourceName(OriginSource source)
        {
            switch (source) {
            case OriginSource::Live:
                return "live";
            case OriginSource::SaveCell:
                return "save-cell";
            case OriginSource::SaveCellLocal:
                return "save-cell-marker";
            case OriginSource::HomeLocation:
                return "home-location";
            case OriginSource::Unresolved:
            default:
                return "unresolved";
            }
        }

        // Where is this visitor, when they are almost certainly not loaded?
        //
        // This is the ordinary case, not an edge one: the beat exists to
        // bring somebody who is ELSEWHERE, and `RoadRoute::ResolveOrigin`
        // reads `GetParentCell()`, which is `return parentCell` and is
        // null for any reference the engine has not attached. It also
        // walks an interior's references looking for a load door, which
        // finds nothing in a cell nobody has loaded. So it answers for the
        // player, who is always loaded, and for a sender standing in the
        // same room -- and for nobody else.
        //
        // Hence a ladder, best first:
        //
        //   Live          where they actually are. Only available while
        //                 they are loaded, which for a visit sender is
        //                 usually not the case.
        //   SaveCell      GetSaveParentCell is the cell the save file
        //                 holds them in, and data.location travels with
        //                 the reference whether or not it has 3D. Exterior
        //                 only -- an unloaded interior still cannot be
        //                 walked for its door.
        //   HomeLocation  the location the record says they belong to, and
        //                 its map marker. Coarsest of the three, and the
        //                 one that answers for somebody asleep in an
        //                 interior on the other side of the province.
        //
        // The rung that answers is logged. A visit staged off HomeLocation
        // is pointing at where the visitor LIVES rather than where they
        // are, which is defensible for a visit and would not be for
        // anything that claimed to track them.
        OriginSource ResolveActorOrigin(const MainThread::Token& mt,
                                        RE::Actor* sender,
                                        const char* who,
                                        RoadRoute::Origin& out)
        {
            out = RoadRoute::ResolveOrigin(mt, sender);
            if (out.valid) {
                return OriginSource::Live;
            }

            auto* saveCell = RoadRoute::CellOf(sender);
            const bool saveCellInterior = saveCell && saveCell->IsInteriorCell();
            if (saveCell && !saveCellInterior) {
                if (auto* ws = saveCell->GetRuntimeData().worldSpace) {
                    out.valid = true;
                    out.worldSpace = ws->GetFormID();
                    out.position = sender->GetPosition();
                    out.viaLoadDoor = false;
                    return OriginSource::SaveCell;
                }
            }

            // Indoors and unloaded, which is where most people are most of
            // the time. The cell's own Location knows the building, and
            // somewhere up its parentLoc chain is a map marker.
            std::string cellTrail;
            if (saveCellInterior && RoadRoute::MarkerFromLocation(saveCell->GetLocation(), out, cellTrail)) {
                logger::debug("VisitArrivalPoint: {} 0x{:08X} placed from their save cell's location ({})",
                              who,
                              sender->GetFormID(),
                              cellTrail);
                return OriginSource::SaveCellLocal;
            }

            // Last resort: whatever Location the record itself files them
            // under, which may be set when no cell answers.
            std::string homeTrail;
            auto* home = sender->GetEditorLocation();
            if (!home) {
                home = sender->GetCurrentLocation();
            }
            if (RoadRoute::MarkerFromLocation(home, out, homeTrail)) {
                logger::debug("VisitArrivalPoint: {} 0x{:08X} placed from the location they belong to ({})",
                              who,
                              sender->GetFormID(),
                              homeTrail);
                return OriginSource::HomeLocation;
            }

            // Everything declined. Say what each rung actually saw, so the
            // next attempt does not have to be another run of the game.
            logger::warn("VisitArrivalPoint: {} 0x{:08X} could not be placed — save_cell={} interior={} "
                         "cell_location_trail='{}' editor_or_current_location={} location_trail='{}'",
                         who,
                         sender->GetFormID(),
                         saveCell != nullptr,
                         saveCellInterior,
                         cellTrail,
                         home != nullptr,
                         homeTrail);
            return OriginSource::Unresolved;
        }

        // The way out of a walled city, as a pair of doors.
        //
        // Skyrim's cities are their own worldspaces, so a visitor living
        // in Tamriel and a player standing in Solitude have coordinates
        // that cannot be compared, let alone routed between. Eight of
        // sixteen dispatches in the first broad run died on exactly
        // that. What joins the two is a door: one reference in the
        // player's worldspace, its partner in the visitor's.
        struct CityGateway
        {
            RE::TESObjectREFR* nearSide = nullptr; // in the player's worldspace
            RE::TESObjectREFR* farSide = nullptr;  // in the visitor's
            RE::NiPoint3 arrival{};                // where the far door puts you
            bool Ok() const
            {
                return nearSide && farSide;
            }
        };

        // Find the nearest door out of `fromWorldSpace` that lands in
        // `toWorldSpace`.
        //
        // Walks the loaded cell grid the same way FineRoads does, since
        // ForEachReferenceInRange has no binding here. Nearest wins: a
        // city with several gates should send the visitor to whichever
        // one the player is actually near.
        CityGateway FindCityGateway(const RE::NiPoint3& playerPos, RE::FormID toWorldSpace)
        {
            CityGateway best;
            float bestDist = -1.0f;

            auto* tes = RE::TES::GetSingleton();
            if (!tes) {
                return best;
            }
            auto* grid = tes->gridCells;
            if (!grid || !grid->cells) {
                return best;
            }

            for (std::uint32_t gx = 0; gx < grid->length; ++gx) {
                for (std::uint32_t gy = 0; gy < grid->length; ++gy) {
                    auto* cell = grid->GetCell(gx, gy);
                    if (!cell || !cell->IsAttached()) {
                        continue;
                    }
                    cell->ForEachReference([&](RE::TESObjectREFR* candidate) {
                        if (!candidate) {
                            return RE::BSContainer::ForEachResult::kContinue;
                        }
                        auto* teleport = candidate->extraList.GetByType<RE::ExtraTeleport>();
                        if (!teleport || !teleport->teleportData) {
                            return RE::BSContainer::ForEachResult::kContinue;
                        }
                        auto linked = teleport->teleportData->linkedDoor.get();
                        auto* farDoor = linked.get();
                        if (!farDoor) {
                            return RE::BSContainer::ForEachResult::kContinue;
                        }
                        auto* farCell = RoadRoute::CellOf(farDoor);
                        if (!farCell || farCell->IsInteriorCell()) {
                            return RE::BSContainer::ForEachResult::kContinue;
                        }
                        auto* farWs = farCell->GetRuntimeData().worldSpace;
                        if (!farWs || farWs->GetFormID() != toWorldSpace) {
                            return RE::BSContainer::ForEachResult::kContinue;
                        }
                        const float d = Dist2D(candidate->GetPosition(), playerPos);
                        if (bestDist < 0.0f || d < bestDist) {
                            bestDist = d;
                            best.nearSide = candidate;
                            best.farSide = farDoor;
                            best.arrival = teleport->teleportData->position;
                        }
                        return RE::BSContainer::ForEachResult::kContinue;
                    });
                }
            }
            return best;
        }

        // Do two worldspaces put a position in the same numbers?
        //
        // A walled city is its own worldspace, but not its own coordinate
        // system. Whiterun's cells sit at grid (4,-2) and its map marker at
        // (19855,-7422), which is the same square of Tamriel the city occupies
        // on the map. The gate proves it: a city gate and the spot it lands
        // you on are one doorway measured from both sides, and across the five
        // walled cities the two readings differ by 26 to 792 units -- the
        // depth of a gateway. Same origin.
        //
        // The parent link is what says so, and NOT the parent-use flags.
        // `kUseLandData` looks like the right question and is not: Markarth
        // does not set it, because it is carved into a cliff and supplies its
        // own landscape rather than drawing Tamriel's -- and Markarth's gate
        // still reads 103 units. Drawing the parent's terrain and measuring
        // from the parent's origin are different claims, and only the second
        // one matters here.
        //
        // So the frame is the root of the parent chain. Tamriel and every city
        // under it share one; Solstheim is its own root, and Apocrypha's root
        // is Solstheim rather than Tamriel, so neither offers a bearing to
        // somebody standing in Skyrim. Which is also just true -- Solstheim is
        // a boat, not a walk.
        RE::TESWorldSpace* GroundFrameOf(RE::FormID worldSpace)
        {
            auto* ws = RE::TESForm::LookupByID<RE::TESWorldSpace>(worldSpace);
            // Depth-capped because a cycle here would hang the beat, and
            // nothing in the game needs more than one hop anyway.
            for (int depth = 0; ws && ws->parentWorld && depth < 8; ++depth) {
                ws = ws->parentWorld;
            }
            return ws;
        }

        bool SharesCoordinateFrame(RE::FormID a, RE::FormID b)
        {
            if (a == b) {
                return true;
            }
            auto* frameA = GroundFrameOf(a);
            return frameA && frameA == GroundFrameOf(b);
        }

        // The deepest place the visitor can be said to come FROM, in
        // numbers the player's worldspace can read.
        //
        // Reached when the two are in different worldspaces and no door
        // joins them in the loaded grid -- the player is not at the city
        // they live in, they are somewhere else in Skyrim entirely. The
        // gate is still the way out, but it is thousands of units away in
        // an unloaded cell and nothing here can find it.
        //
        // It does not need finding. What the route wants from the sender
        // is a bearing, and `MarkerFromLocation` already walks their
        // location chain outward and stops at the first marker it meets --
        // the deepest one, so Whiterun rather than Whiterun Hold. That
        // marker stands in the city's own worldspace and is no less a
        // Tamriel coordinate for it.
        //
        // This is a PATHING anchor and nothing else. Where the visitor
        // actually stands, and where they are put back afterwards, is the
        // sender's own origin, which this does not touch.
        bool HomeMarkerBearing(RE::Actor* sender,
                               RE::FormID senderWorldSpace,
                               RE::FormID playerWorldSpace,
                               RoadRoute::Origin& out,
                               std::string& trail)
        {
            if (!sender) {
                return false;
            }

            // The worldspace's own Location, which is the rung that answers
            // for anybody standing in the street.
            //
            // A city cell does not name its location: all 113 exterior cells
            // of WhiterunWorld leave XLCN empty, and the field is set once, on
            // the worldspace record, where it reads WhiterunLocation. So
            // asking the cell works for the sender indoors at the Bannered
            // Mare and for nobody outside it, which is the wrong half.
            auto* worldLocation = [senderWorldSpace]() -> RE::BGSLocation* {
                auto* ws = RE::TESForm::LookupByID<RE::TESWorldSpace>(senderWorldSpace);
                return ws ? ws->location : nullptr;
            }();

            RoadRoute::Origin marker;
            auto* saveCell = RoadRoute::CellOf(sender);
            const bool found = (saveCell && RoadRoute::MarkerFromLocation(saveCell->GetLocation(), marker, trail))
                               || RoadRoute::MarkerFromLocation(worldLocation, marker, trail)
                               || RoadRoute::MarkerFromLocation(sender->GetEditorLocation(), marker, trail)
                               || RoadRoute::MarkerFromLocation(sender->GetCurrentLocation(), marker, trail);
            if (!found) {
                return false;
            }
            if (!SharesCoordinateFrame(marker.worldSpace, playerWorldSpace)) {
                trail += "[off-frame]";
                return false;
            }

            out = marker;
            // Read in the player's worldspace from here on, because that
            // is the graph the route is planned over.
            out.worldSpace = playerWorldSpace;
            return true;
        }

        struct Endpoints
        {
            bool resolved = false;
            RoadRoute::Origin sender;
            RoadRoute::Origin player;
            OriginSource playerSource = OriginSource::Unresolved;
            // Which way the player is facing, for the unseen-arrival
            // test. Captured here with the position because both are
            // engine reads and this is the one main-thread hop that
            // already does them.
            float playerAngleZ = 0.0f;
            OriginSource senderSource = OriginSource::Unresolved;
        };
    } // namespace

    const char* TierName(Tier tier)
    {
        switch (tier) {
        case Tier::Chain:
            return "chain";
        case Tier::CityApproach:
            return "city-approach";
        case Tier::CityGate:
            return "city-gate";
        case Tier::Doorstep:
            return "doorstep";
        case Tier::None:
        default:
            return "none";
        }
    }

    Result Find(const PluginThread::Token& pt, RE::Actor* sender, RE::Actor* player)
    {
        Result result;
        if (!sender || !player) {
            logger::warn("VisitArrivalPoint: no sender or no player");
            return result;
        }

        const auto& cfg = Settings::Get();
        const float minDist = static_cast<float>(std::max(1, cfg.visitMarkerMinDistanceUnits));
        const float maxDist = std::max(minDist + 1.0f, static_cast<float>(cfg.visitMarkerMaxDistanceUnits));
        const float coverRadius = static_cast<float>(std::max(0, cfg.visitArrivalCoverRadiusUnits));
        const RE::FormID senderId = sender->GetFormID();

        // Engine state: both ends of the route, resolved through load
        // doors where either party is indoors.
        auto ends = MainThread::Run(pt, [sender, player](const MainThread::Token& mt) {
            Endpoints out;
            out.senderSource = ResolveActorOrigin(mt, sender, "sender", out.sender);
            out.playerSource = ResolveActorOrigin(mt, player, "player", out.player);
            out.playerAngleZ = player->data.angle.z;
            out.resolved = out.sender.valid && out.player.valid;
            return out;
        });

        if (!ends.resolved) {
            logger::warn("VisitArrivalPoint: sender=0x{:08X} tier=none — origin unresolved "
                         "(sender_valid={} via={} | player_valid={} via={}). Nothing in the record or the "
                         "save could say where one of them is.",
                         senderId,
                         ends.sender.valid,
                         OriginSourceName(ends.senderSource),
                         ends.player.valid,
                         OriginSourceName(ends.playerSource));
            return result;
        }
        // The player is inside a building, and the visitor waits at the
        // door they will come out of.
        //
        // Nothing is searched for, because nothing can be. Standing in an
        // interior empties the exterior cell grid, and every gate the
        // search runs reads it: `IsStandable` asks `TES::GetLandHeight`,
        // which has no landscape to answer from, so all 45 bearing
        // samples come back off-navmesh and no point anywhere outdoors
        // can be validated. The cover raycast has nothing loaded to hit
        // either.
        //
        // None of that matters, because the answer needs no validating.
        // `teleportData->position` is where the engine itself puts the
        // player every time they walk out of this door, so it is ground
        // somebody stands on by construction. The visitor waits there and
        // the player finds them on the way out.
        if (ends.player.exteriorDoor != 0) {
            result.tier = Tier::Doorstep;
            result.point = ends.player.position;
            result.placementAnchor = ends.player.exteriorDoor;
            logger::info("VisitArrivalPoint: sender=0x{:08X} tier=doorstep at ({:.0f},{:.0f},{:.0f}) — the "
                         "player is indoors, so the visitor waits at the far side of their own door "
                         "0x{:08X} in ws=0x{:08X}",
                         senderId,
                         result.point.x,
                         result.point.y,
                         result.point.z,
                         result.placementAnchor,
                         ends.player.worldSpace);
            return result;
        }

        // Set when the two ends were in different worldspaces and the
        // visitor's end has been re-read off their home's map marker
        // instead, which puts both in the player's numbers and lets the
        // ordinary road search below do the rest.
        std::string homeMarkerTrail;
        bool viaHomeMarker = false;

        if (ends.sender.worldSpace != ends.player.worldSpace) {
            // A walled city, almost always. The two are not in the
            // same coordinate system, so nothing can be routed between
            // them directly -- but a door joins them, and the visitor can
            // walk through it like anybody else.
            // Measured from where the player IS IN THAT WORLDSPACE,
            // which is not always where they are standing. An indoor
            // player with a load door has already been answered by the
            // doorstep tier above; what reaches here is one whose
            // interior has no way out and resolved to a map marker
            // instead. Their own coordinates would mean nothing against
            // the gate's, so the marker is what every city decision below
            // is made from.
            const RE::NiPoint3& cityAnchorPos = ends.player.position;
            const bool playerIndoors = ends.player.viaLoadDoor;

            const auto gateway = MainThread::Run(
                pt, [&](const MainThread::Token&) { return FindCityGateway(cityAnchorPos, ends.sender.worldSpace); });
            if (!gateway.Ok()) {
                // No gate in the loaded grid, which is the ordinary case
                // rather than an odd one: it means the player is not at
                // the city, they are out in Skyrim somewhere. Every
                // dispatch whose visitor lived behind a set of walls
                // declined here, and there are five such cities holding a
                // large share of everybody worth visiting.
                //
                // The gate is not needed to answer. Anchor the visitor on
                // their home's map marker, which is in the same ground
                // frame as the player, and fall through to the road
                // search that handles every other visitor.
                const RE::FormID senderWorldSpace = ends.sender.worldSpace;
                RoadRoute::Origin viaMarker;
                viaHomeMarker = MainThread::Run(pt, [&](const MainThread::Token&) {
                    return HomeMarkerBearing(
                        sender, senderWorldSpace, ends.player.worldSpace, viaMarker, homeMarkerTrail);
                });
                if (!viaHomeMarker) {
                    logger::info("VisitArrivalPoint: sender=0x{:08X} tier=none — different worldspaces "
                                 "(sender=0x{:08X} player=0x{:08X}), no door between them in the loaded "
                                 "cells, and no home marker sharing the player's ground ('{}')",
                                 senderId,
                                 senderWorldSpace,
                                 ends.player.worldSpace,
                                 homeMarkerTrail);
                    return result;
                }

                ends.sender = viaMarker;
                logger::info("VisitArrivalPoint: sender=0x{:08X} lives in ws=0x{:08X} with no gate loaded; "
                             "routing from their home marker at ({:.0f},{:.0f}) in the player's ws=0x{:08X} "
                             "instead ({})",
                             senderId,
                             senderWorldSpace,
                             ends.sender.position.x,
                             ends.sender.position.y,
                             ends.player.worldSpace,
                             homeMarkerTrail);
            } else {
                const auto gatePos = gateway.nearSide->GetPosition();
                logger::info("VisitArrivalPoint: sender=0x{:08X} across worldspaces "
                             "(sender=0x{:08X} player=0x{:08X}); player_indoors={} measured from "
                             "({:.0f},{:.0f}); gate at ({:.0f},{:.0f}), {:.0f}u out",
                             senderId,
                             ends.sender.worldSpace,
                             ends.player.worldSpace,
                             playerIndoors,
                             cityAnchorPos.x,
                             cityAnchorPos.y,
                             gatePos.x,
                             gatePos.y,
                             Dist2D(gatePos, cityAnchorPos));

                // Inside the walls first. Somewhere between the player and
                // the gate reads as the visitor having already come through
                // it, which is a shorter and more natural walk than watching
                // them traverse the gate.
                GateTally cityTally;
                auto inside = MainThread::Run(pt, [&](const MainThread::Token&) {
                    return SampleBearingArc(
                        cityAnchorPos, ends.playerAngleZ, gatePos, minDist, maxDist, coverRadius, cityTally);
                });
                if (!inside.empty()) {
                    result.tier = Tier::CityApproach;
                    result.point = inside.front();
                    result.fallbacks.assign(inside.begin() + 1, inside.end());
                    // The point is in the player's worldspace but not
                    // necessarily in the player's CELL: a player still inside
                    // the inn would have the marker built in the inn. The
                    // city side of the gate is the nearest reference known to
                    // be standing on the ground this point is on.
                    if (playerIndoors) {
                        result.placementAnchor = gateway.nearSide->GetFormID();
                    }
                    logger::info("VisitArrivalPoint: sender=0x{:08X} tier=city-approach at "
                                 "({:.0f},{:.0f},{:.0f}) inside the walls, anchored on 0x{:08X} | city[{}]",
                                 senderId,
                                 result.point.x,
                                 result.point.y,
                                 result.point.z,
                                 result.placementAnchor,
                                 cityTally.Describe());
                    return result;
                }

                // Nothing usable inside -- most often because the player can
                // see the whole way to the gate. So put the visitor on the
                // far side of it and let them walk in, which is what the
                // player would expect to see anyway.
                result.tier = Tier::CityGate;
                result.point = gateway.arrival;
                result.point.z += kGroundClearanceUnits;
                result.placementAnchor = gateway.farSide->GetFormID();

                // Unless the player is standing at the gate, in which case
                // "outside it" is a few paces away. A player 1,168 units
                // inside Whiterun's gate got a visitor placed 1,151 units off
                // and greeted them 1.3 seconds after arming -- no walk, no
                // arrival, somebody simply there.
                //
                // Keep walking out along the same line the visitor would come
                // in on until the floor is satisfied. City worldspaces are in
                // Tamriel's own coordinates (see
                // docs/engine-findings/city-worldspaces-share-tamriels-origin.md),
                // so the player's position and the gate's landing can be
                // measured against each other and extended past one another
                // without conversion.
                const float gateOut = Dist2D(result.point, cityAnchorPos);
                std::vector<RE::NiPoint3> pushed;
                if (gateOut < minDist) {
                    const float dx = result.point.x - cityAnchorPos.x;
                    const float dy = result.point.y - cityAnchorPos.y;
                    const float span = std::sqrt(dx * dx + dy * dy);
                    if (span > 1.0f) {
                        const float ux = dx / span;
                        const float uy = dy / span;
                        // Furthest first, so the push that satisfies the
                        // floor is the one taken and the shorter ones fall to
                        // the escort behind it.
                        //
                        // Accepted on distance rather than validated, which is
                        // the rule the chain already applies to a point outside
                        // the loaded grid. FindCityGateway searches the LOADED
                        // grid for a door whose far side lands in the other
                        // worldspace, so this arrival is by construction in a
                        // worldspace that is not loaded -- GetCell resolves its
                        // coordinates against the grid that IS, which out
                        // beyond the city walls holds no ground at all.
                        //
                        // Asking anyway failed all four probes on a measured
                        // visit and left the arrival at the gate, 1,484 units
                        // from a player with a 2,000-unit floor. Nothing was
                        // wrong with the probes; the question has no answer
                        // from in here.
                        const float needed = minDist - gateOut;
                        for (const float fraction : {1.0f, 0.75f, 0.5f, 0.25f}) {
                            const float reach = needed * fraction;
                            pushed.push_back(
                                RE::NiPoint3{result.point.x + ux * reach, result.point.y + uy * reach, result.point.z});
                        }
                    }
                }
                if (!pushed.empty()) {
                    // The gate itself stays on the end of the ladder. It is
                    // where the engine puts anybody walking through, so it is
                    // the one position out here the engine vouches for -- and
                    // so the one to reach for last rather than first.
                    pushed.push_back(result.point);
                    result.point = pushed.front();
                    result.fallbacks.assign(pushed.begin() + 1, pushed.end());
                }

                logger::info("VisitArrivalPoint: sender=0x{:08X} tier=city-gate at ({:.0f},{:.0f},{:.0f}) "
                             "outside the gate, anchored on 0x{:08X}, {:.0f}u out (gate was {:.0f}u, "
                             "floor {:.0f}u), {} fallback(s) | city[{}]",
                             senderId,
                             result.point.x,
                             result.point.y,
                             result.point.z,
                             result.placementAnchor,
                             Dist2D(result.point, cityAnchorPos),
                             gateOut,
                             minDist,
                             result.fallbacks.size(),
                             cityTally.Describe());
                return result;
            }
        }

        // How the visitor's end was arrived at, for the two log lines that
        // close this out. After a re-anchor the origin rung alone is
        // misleading: it names where they were read from, not the marker
        // the route was actually planned against.
        const std::string senderVia =
            viaHomeMarker ? std::string(OriginSourceName(ends.senderSource)) + "->home-marker(" + homeMarkerTrail + ")"
                          : std::string(OriginSourceName(ends.senderSource));

        // Where the player IS IN THIS WORLDSPACE, which is not always
        // where they are standing.
        //
        // `player->GetPosition()` is not it. Inside a building that is
        // interior coordinates -- small numbers local to the cell that
        // mean nothing against a road. Feeding them to
        // `FindNearestNode` picks whatever coarse node happens to sit
        // near the worldspace origin, and every dispatch with the player
        // indoors chased a corridor ten thousand units away in the wrong
        // direction because of it. `ResolveOrigin` has already walked
        // them out their own front door; that doorstep is the anchor.
        //
        // Outdoors the two are the same value, so nothing changes there.
        const RE::NiPoint3& anchorPos = ends.player.position;

        // Pure queries over both graphs — deliberately NOT inside a
        // main-thread hop. Only the grid read is an engine call.
        const auto loadedGrid =
            MainThread::Run(pt, [](const MainThread::Token& mt) { return ApproachChain::ReadLoadedGrid(mt); });
        const auto chain = ApproachChain::Build(loadedGrid, ends.player.worldSpace, ends.sender.position, anchorPos);
        if (!chain.valid) {
            // The visitor-to-player line is meant to make this
            // unreachable. Logged loudly rather than quietly declined,
            // because reaching it means the graph was built wrong rather
            // than that the world was unhelpful.
            logger::warn("VisitArrivalPoint: sender=0x{:08X} no approach chain in ws=0x{:08X} — declining",
                         senderId,
                         ends.player.worldSpace);
            return result;
        }

        const auto fine = FineRoads::Snapshot();
        const int hopRadius = std::max(0, cfg.visitChainHopRadius);
        const auto candidates = GatherCandidates(chain, fine, hopRadius, anchorPos);

        GateTally tally;
        const auto kept = MainThread::Run(pt, [&](const MainThread::Token&) {
            return WalkChain(candidates,
                             anchorPos,
                             ends.playerAngleZ,
                             minDist,
                             coverRadius,
                             cfg.visitArrivalAllowCoarseBearing,
                             tally);
        });

        const Tier tier = kept.empty() ? Tier::None : Tier::Chain;

        if (tier == Tier::None) {
            logger::info("VisitArrivalPoint: sender=0x{:08X} tier=none — ws=0x{:08X} "
                         "home=({:.0f},{:.0f}) via={} player=({:.0f},{:.0f},{:.0f}){} min={:.0f} "
                         "cover_r={:.0f} chain={} points cost={:.0f} candidates={} outside_grid_allowed={} "
                         "| {}",
                         senderId,
                         ends.player.worldSpace,
                         ends.sender.position.x,
                         ends.sender.position.y,
                         senderVia,
                         ends.player.position.x,
                         ends.player.position.y,
                         anchorPos.z,
                         ends.player.viaLoadDoor ? " via-door" : "",
                         minDist,
                         coverRadius,
                         chain.points.size(),
                         chain.cost,
                         candidates.size(),
                         cfg.visitArrivalAllowCoarseBearing,
                         tally.Describe());
            return result;
        }

        result.tier = tier;
        result.pointClass = kept.front().cls;
        result.point = kept.front().standing;
        for (std::size_t i = 1; i < kept.size(); ++i) {
            result.fallbacks.push_back(kept[i].standing);
        }

        // Where the visitor lives, and where they were actually put, as
        // bearings from the player.
        //
        // These two are NOT expected to agree, and the gap between them is
        // not an error term. The arrival sits on the routed path toward
        // the visitor, so on a road that bends -- or a north-south road
        // reached by somebody who lives due east -- a wide difference is
        // the road being followed rather than ignored. An earlier version
        // logged the delta as "off_by", which reads as a fault and was
        // duly mistaken for one. Both bearings are kept because they
        // locate the arrival on a map; `tier` is what says whether the
        // route was honoured.
        const float homeBearing =
            ToDegrees(std::atan2(ends.sender.position.y - anchorPos.y, ends.sender.position.x - anchorPos.x));
        const float pointBearing = ToDegrees(std::atan2(result.point.y - anchorPos.y, result.point.x - anchorPos.x));

        logger::info("VisitArrivalPoint: sender=0x{:08X} tier={} class={} — ws=0x{:08X} home=({:.0f},{:.0f}) "
                     "via={} player=({:.0f},{:.0f},{:.0f}){} point=({:.0f},{:.0f},{:.0f}) dist={:.0f}u "
                     "dz={:+.0f}u bearing_home={:.0f}deg bearing_arrival={:.0f}deg in_grid={} hops={} "
                     "chain={} points cost={:.0f} candidates={} fallbacks={} | {}",
                     senderId,
                     TierName(tier),
                     ApproachChain::PointClassName(result.pointClass),
                     ends.player.worldSpace,
                     ends.sender.position.x,
                     ends.sender.position.y,
                     senderVia,
                     anchorPos.x,
                     anchorPos.y,
                     anchorPos.z,
                     ends.player.viaLoadDoor ? " via-door" : "",
                     result.point.x,
                     result.point.y,
                     result.point.z,
                     Dist2D(result.point, anchorPos),
                     result.point.z - anchorPos.z,
                     homeBearing,
                     pointBearing,
                     kept.front().insideGrid,
                     kept.front().hopCount,
                     chain.points.size(),
                     chain.cost,
                     candidates.size(),
                     result.fallbacks.size(),
                     tally.Describe());

        // Where the escort will send them if they cannot walk it, in the
        // order it will try. StuckRecovery logs each warp as it happens, but
        // only this says what the supply was to begin with.
        for (std::size_t i = 0; i < result.fallbacks.size(); ++i) {
            logger::debug("VisitArrivalPoint: fallback {} at ({:.0f},{:.0f},{:.0f}), {:.0f}u out",
                          i,
                          result.fallbacks[i].x,
                          result.fallbacks[i].y,
                          result.fallbacks[i].z,
                          Dist2D(result.fallbacks[i], anchorPos));
        }
        return result;
    }
} // namespace NarrativeEngine::VisitArrivalPoint
