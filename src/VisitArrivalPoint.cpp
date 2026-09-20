#include <VisitArrivalPoint.h>

#include <CameraVisibility.h>
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

        // Tier 2's arc around the bearing home, and how finely it is
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

        // How far out a coarse node has to be before the bearing to it
        // says anything about which way home is.
        //
        // The coarse graph is sampled at one node per navmesh cell, so
        // `FindNearestNode` can hand back a node anywhere within roughly
        // half a cell diagonal of the player -- and which side it lands
        // on is decided by where the road network happens to run, not by
        // where the visitor lives. Aiming the arc at it produced
        // arrivals 115, 159 and 207 degrees off the direction home in
        // one run.
        //
        // A whole cell is the smallest separation at which the answer
        // stops being an artefact of the sampling, so that is the floor.
        // It is deliberately larger than `kBearingMinSeparationUnits`,
        // which guards a different thing: that the visitor does not
        // appear on top of the player.
        constexpr float kBearingAimMinUnits = 4096.0f;

        // The stretch of road a visitor would actually walk in on.
        //
        // Routing straight from the player to the sender does not give
        // this. `RoadRoute::Route` picks which frontier to hand off at by
        // total journey cost, and its own header records that the coarse
        // half of that sum underestimates in a way nobody has verified —
        // so when the forward branch has no cover, the search will take
        // the rearward one and the visitor walks in from behind. Two of
        // six arrivals in the first broad run did exactly that.
        //
        // So the corridor is built the other way round, as the coarse
        // route decides it:
        //
        //   1. Coarse path from the sender's origin to the player. This
        //      is the province-scale route and has no frontier heuristic
        //      in it.
        //   2. Walk back from the player's end of that path to the first
        //      node genuinely clear of them. That node is where the
        //      approach comes THROUGH.
        //   3. Route the player to THAT, not to the sender. The
        //      destination is now close, so Route takes its
        //      destination-within-fine branch and returns a plain fine
        //      path with no handoff to choose.
        //
        // What comes back is the road between the player and the way in,
        // which is the only stretch a candidate has any business being
        // on.
        //
        // A rejected alternative, recorded because it is the obvious one:
        // requiring each candidate to be closer to the sender than the
        // player is. Roads run away before they curve back, and that test
        // refuses every one that does.
        bool CorridorTarget(RE::FormID worldSpace,
                            const RE::NiPoint3& senderPos,
                            const RE::NiPoint3& playerPos,
                            RE::NiPoint3& out,
                            RE::NiPoint3& aim,
                            bool& haveAim,
                            std::string& why)
        {
            haveAim = false;
            const auto fromSender = TravelGraph::FindNearestNode(worldSpace, senderPos.x, senderPos.y);
            const auto toPlayer = TravelGraph::FindNearestNode(worldSpace, playerPos.x, playerPos.y);
            if (fromSender == TravelGraph::kInvalidNode || toPlayer == TravelGraph::kInvalidNode) {
                why = "no coarse node for one end";
                return false;
            }
            if (fromSender == toPlayer) {
                why = "both ends share one coarse node";
                return false;
            }

            const auto path = TravelGraph::FindPath(fromSender, toPlayer);
            if (path.empty()) {
                why = "no coarse route between them";
                return false;
            }

            // Backwards from the player's end: the first node far enough
            // out to point somewhere is the way in.
            //
            // `aim` is a second, stricter pick off the same path. The way
            // in wants to be NEAR, so that routing to it keeps the plan
            // inside fine coverage; a bearing wants to be FAR, so that it
            // describes the road home rather than the nearest scrap of
            // road. One walk answers both.
            bool haveWayIn = false;
            for (auto it = path.rbegin(); it != path.rend(); ++it) {
                const auto* node = TravelGraph::GetNode(*it);
                if (!node) {
                    continue;
                }
                const RE::NiPoint3 at{node->x, node->y, node->z};
                const float away = Dist2D(at, playerPos);
                if (!haveWayIn && away >= kBearingMinSeparationUnits) {
                    out = at;
                    haveWayIn = true;
                }
                // Never the player's own nearest node, however far off it
                // sits: it says where the network is, not where home is.
                if (it != path.rbegin() && away >= kBearingAimMinUnits) {
                    aim = at;
                    haveAim = true;
                    break;
                }
            }

            if (!haveAim) {
                // Nothing on the route is far enough to be a direction.
                // The visitor's own origin is then the honest answer --
                // a straight line home, which ignores the road but does
                // not point away from it.
                aim = senderPos;
                haveAim = true;
            }
            if (haveWayIn) {
                why = "ok";
                return true;
            }
            why = "every coarse node on the route sits on top of the player";
            return false;
        }

        // Largest elevation difference from the player a Tier 2
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
        // used even with no cover at all.
        //
        // Cover exists to hide the instant somebody appears. If the
        // player is not looking that way, that instant is already
        // hidden, and insisting on geometry as well turns "no visit" into
        // the common outcome on open ground.
        constexpr float kUnseenDistanceUnits = 3000.0f;

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
                       + " notLevel=" + std::to_string(notLevel) + " inView=" + std::to_string(inView)
                       + " unseen=" + std::to_string(unseen)
                       + " survived=" + std::to_string(considered - tooNear - offNavmesh - notLevel - inView);
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
            return CameraVisibility::IsPositionBehindCover(pos, kCoverProbeHeightUnits, coverRadius);
        }

        // ---- Tier 1 -------------------------------------------------
        //
        // Walk the fine path outward from the player and keep every node
        // that clears the gates, in road order.
        //
        // The whole path is scanned rather than stopping at the first
        // node past the band: a road can curve back toward the player,
        // so road order and straight-line distance do not rise together
        // and an early exit would miss usable ground beyond a bend.
        std::vector<RE::NiPoint3> WalkFinePath(const std::vector<RE::NiPoint3>& finePath,
                                               const RE::NiPoint3& playerPos,
                                               float playerAngleZ,
                                               float minDist,
                                               float maxDist,
                                               float coverRadius,
                                               GateTally& tally)
        {
            // Two pools, because the band's ceiling is a preference and
            // not a rule. Anything inside it wins; a point past it is
            // held back and used only if the band yielded nothing, which
            // beats declining the visit over a walk being a bit long.
            std::vector<RE::NiPoint3> inBand;
            std::vector<RE::NiPoint3> beyondBand;

            for (const auto& node : finePath) {
                const float distance = Dist2D(node, playerPos);
                tally.Saw(distance);
                if (distance < minDist) {
                    ++tally.tooNear;
                    continue;
                }
                if (distance > kMaxReachUnits) {
                    // Past the loaded grid there is no actor to place.
                    ++tally.tooFar;
                    continue;
                }
                // The graph is a snapshot and cells unload out from
                // under it, so being on navmesh is re-checked rather
                // than inherited from the triangle the node came from.
                if (!StuckRecovery::IsOnNavmesh(node)) {
                    ++tally.offNavmesh;
                    continue;
                }
                RE::NiPoint3 standing = node;
                standing.z += kGroundClearanceUnits;

                const char* passedBy = "cover";
                if (!BehindCover(standing, coverRadius)) {
                    // No geometry to hide behind. Usable anyway if the
                    // player is both far enough away and facing
                    // elsewhere -- on open ground that is the difference
                    // between a visit and no visit.
                    if (distance < kUnseenDistanceUnits || !OutsidePlayerView(standing, playerPos, playerAngleZ)) {
                        ++tally.inView;
                        continue;
                    }
                    ++tally.unseen;
                    passedBy = "unseen";
                }

                auto& pool = (distance <= maxDist) ? inBand : beyondBand;
                const std::size_t before = pool.size();
                AppendSeparated(pool, standing);
                if (pool.size() != before) {
                    // Elevation is in here because the cover gate is at
                    // its least trustworthy uphill: rays that graze a
                    // slope read as blocked while the figure standing on
                    // it is skylined from below.
                    logger::debug("VisitArrivalPoint: candidate ({:.0f},{:.0f},{:.0f}) kept — {:.0f}u out, "
                                  "{:+.0f}u in elevation, passed by {}, {}",
                                  standing.x,
                                  standing.y,
                                  standing.z,
                                  distance,
                                  standing.z - playerPos.z,
                                  passedBy,
                                  (distance <= maxDist) ? "in band" : "past the band");
                }
                if (inBand.size() > kMaxFallbacks) {
                    break;
                }
            }

            if (!inBand.empty()) {
                return inBand;
            }
            return beyondBand;
        }

        // ---- Tier 2 -------------------------------------------------

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

        // The first coarse node on the route that is far enough from the
        // player to mean a direction. Returns false when the coarse path
        // never gets clear of them.
        bool BearingHome(const std::vector<std::size_t>& coarsePath, const RE::NiPoint3& playerPos, RE::NiPoint3& out)
        {
            for (const auto index : coarsePath) {
                const auto* node = TravelGraph::GetNode(index);
                if (!node) {
                    continue;
                }
                const RE::NiPoint3 at{node->x, node->y, node->z};
                if (Dist2D(at, playerPos) >= kBearingMinSeparationUnits) {
                    out = at;
                    return true;
                }
            }
            return false;
        }

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
        case Tier::FineRoad:
            return "fine-road";
        case Tier::CoarseBearing:
            return "coarse-bearing";
        case Tier::CityApproach:
            return "city-approach";
        case Tier::CityGate:
            return "city-gate";
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
        const auto ends = MainThread::Run(pt, [sender, player](const MainThread::Token& mt) {
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
        if (ends.sender.worldSpace != ends.player.worldSpace) {
            // A walled city, almost always. The two are not in the
            // same coordinate system, so nothing can be routed between
            // them directly -- but a door joins them, and the visitor can
            // walk through it like anybody else.
            // Measured from where the player IS IN THAT WORLDSPACE,
            // which is not always where they are standing. A player in a
            // city inn has interior coordinates that mean nothing against
            // the gate's; `ResolveOrigin` has already walked them out
            // their own front door, and that doorstep is the position
            // every city decision below is made from.
            const RE::NiPoint3& cityAnchorPos = ends.player.position;
            const bool playerIndoors = ends.player.viaLoadDoor;

            const auto gateway = MainThread::Run(
                pt, [&](const MainThread::Token&) { return FindCityGateway(cityAnchorPos, ends.sender.worldSpace); });
            if (!gateway.Ok()) {
                logger::info("VisitArrivalPoint: sender=0x{:08X} tier=none — different worldspaces "
                             "(sender=0x{:08X} player=0x{:08X}) and no door between them in the loaded "
                             "cells",
                             senderId,
                             ends.sender.worldSpace,
                             ends.player.worldSpace);
                return result;
            }

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
            logger::info("VisitArrivalPoint: sender=0x{:08X} tier=city-gate at ({:.0f},{:.0f},{:.0f}) "
                         "outside the gate, anchored on 0x{:08X} | city[{}]",
                         senderId,
                         result.point.x,
                         result.point.y,
                         result.point.z,
                         result.placementAnchor,
                         cityTally.Describe());
            return result;
        }

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
        // main-thread hop.
        //
        // Route toward the corridor's way-in rather than toward the
        // sender directly. See CorridorTarget: routing at the sender
        // lets Route's frontier heuristic pick the branch behind the
        // player, and it did.
        RE::NiPoint3 corridor{};
        RE::NiPoint3 corridorAim{};
        bool haveCorridorAim = false;
        std::string corridorWhy;
        const bool haveCorridor = CorridorTarget(ends.player.worldSpace,
                                                 ends.sender.position,
                                                 anchorPos,
                                                 corridor,
                                                 corridorAim,
                                                 haveCorridorAim,
                                                 corridorWhy);
        const RE::NiPoint3 routeTo = haveCorridor ? corridor : ends.sender.position;
        if (!haveCorridor) {
            // Falling back to the sender's own position restores the old
            // behaviour, frontier heuristic and all. Logged, because a
            // visit that arrives from a strange direction wants this line
            // to explain itself.
            logger::debug("VisitArrivalPoint: sender=0x{:08X} no approach corridor ({}) — routing at the "
                          "sender directly",
                          senderId,
                          corridorWhy);
        }
        const auto plan = RoadRoute::Route(ends.player.worldSpace, ends.player.position, routeTo);

        GateTally fineTally;
        GateTally bearingTally;
        std::vector<RE::NiPoint3> kept;
        Tier tier = Tier::None;

        if (plan.valid && !plan.finePath.empty()) {
            kept = MainThread::Run(pt, [&](const MainThread::Token&) {
                return WalkFinePath(
                    plan.finePath, anchorPos, ends.playerAngleZ, minDist, maxDist, coverRadius, fineTally);
            });
            if (!kept.empty()) {
                tier = Tier::FineRoad;
            }
        }

        if (tier == Tier::None) {
            // What the bearing aims at, best first.
            //
            // The corridor's way-in comes first because it is the better
            // answer AND because it is usually the only one. Routing at
            // the corridor target puts the destination inside fine
            // coverage, which is the branch of `Route` that returns an
            // EMPTY coarsePath -- so on the old ordering the bearing
            // fallback had nothing to aim at in exactly the case the
            // corridor had just succeeded. Rorikstead and Loreius Farm
            // both declined every visit that way: a fine path of three
            // and fifteen nodes, exhausted, and then no fallback,
            // although `CorridorTarget` had a perfectly good coarse node
            // on the road home the whole time.
            RE::NiPoint3 toward{};
            bool haveToward = false;
            const char* towardSource = "none";
            if (haveCorridorAim) {
                toward = corridorAim;
                haveToward = true;
                towardSource = "corridor";
            } else if (plan.valid && BearingHome(plan.coarsePath, anchorPos, toward)) {
                haveToward = true;
                towardSource = "coarse-route";
            }

            if (!cfg.visitArrivalAllowCoarseBearing) {
                logger::info("VisitArrivalPoint: sender=0x{:08X} no fine-road point and the bearing fallback "
                             "is switched off",
                             senderId);
            } else if (!haveToward) {
                logger::info("VisitArrivalPoint: sender=0x{:08X} no usable bearing — no approach corridor ({}) "
                             "and none of the {} coarse node(s) on the route is {:.0f}u clear of the player",
                             senderId,
                             corridorWhy,
                             plan.coarsePath.size(),
                             kBearingMinSeparationUnits);
            } else {
                logger::info("VisitArrivalPoint: sender=0x{:08X} falling back to a bearing toward "
                             "({:.0f},{:.0f}), {:.0f}u out, from the {}",
                             senderId,
                             toward.x,
                             toward.y,
                             Dist2D(toward, anchorPos),
                             towardSource);
                kept = MainThread::Run(pt, [&](const MainThread::Token&) {
                    return SampleBearingArc(
                        anchorPos, ends.playerAngleZ, toward, minDist, maxDist, coverRadius, bearingTally);
                });
                if (!kept.empty()) {
                    tier = Tier::CoarseBearing;
                }
            }
        }

        if (tier == Tier::None) {
            logger::info("VisitArrivalPoint: sender=0x{:08X} tier=none — ws=0x{:08X} "
                         "home=({:.0f},{:.0f}) via={} player=({:.0f},{:.0f},{:.0f}){} band=[{:.0f},{:.0f}] "
                         "cover_r={:.0f} "
                         "plan_valid={} within_fine={} fine_nodes={} coarse_nodes={} coarse_allowed={} "
                         "| fine[{}] | bearing[{}]",
                         senderId,
                         ends.player.worldSpace,
                         ends.sender.position.x,
                         ends.sender.position.y,
                         OriginSourceName(ends.senderSource),
                         ends.player.position.x,
                         ends.player.position.y,
                         anchorPos.z,
                         ends.player.viaLoadDoor ? " via-door" : "",
                         minDist,
                         maxDist,
                         coverRadius,
                         plan.valid,
                         plan.destinationWithinFine,
                         plan.finePath.size(),
                         plan.coarsePath.size(),
                         cfg.visitArrivalAllowCoarseBearing,
                         fineTally.Describe(),
                         bearingTally.Describe());
            return result;
        }

        result.tier = tier;
        result.point = kept.front();
        result.fallbacks.assign(kept.begin() + 1, kept.end());

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

        logger::info("VisitArrivalPoint: sender=0x{:08X} tier={} — ws=0x{:08X} home=({:.0f},{:.0f}) via={} "
                     "player=({:.0f},{:.0f},{:.0f}){} point=({:.0f},{:.0f},{:.0f}) dist={:.0f}u dz={:+.0f}u "
                     "bearing_home={:.0f}deg bearing_arrival={:.0f}deg within_fine={} "
                     "fine_nodes={} coarse_nodes={} fallbacks={} | fine[{}] | bearing[{}]",
                     senderId,
                     TierName(tier),
                     ends.player.worldSpace,
                     ends.sender.position.x,
                     ends.sender.position.y,
                     OriginSourceName(ends.senderSource),
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
                     plan.destinationWithinFine,
                     plan.finePath.size(),
                     plan.coarsePath.size(),
                     result.fallbacks.size(),
                     fineTally.Describe(),
                     bearingTally.Describe());

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
