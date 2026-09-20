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
                            std::string& why)
        {
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
            for (auto it = path.rbegin(); it != path.rend(); ++it) {
                const auto* node = TravelGraph::GetNode(*it);
                if (!node) {
                    continue;
                }
                const RE::NiPoint3 at{node->x, node->y, node->z};
                if (Dist2D(at, playerPos) >= kBearingMinSeparationUnits) {
                    out = at;
                    why = "ok";
                    return true;
                }
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
                }

                auto& pool = (distance <= maxDist) ? inBand : beyondBand;
                AppendSeparated(pool, standing);
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

        // Upper bound on parentLoc traversal, matching AlphaCanon's. A
        // room's Location rarely carries a map marker; the building or the
        // settlement above it does.
        constexpr int kMaxParentDepth = 16;

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

        // Which cell a reference belongs to, whether or not anything has
        // loaded it.
        //
        // `GetParentCell()` is `return parentCell` and is null for anything
        // unattached. That is the single mistake this module has now made
        // twice -- first on the visitor, then on the map marker it climbed
        // to in order to place the visitor -- so the read lives in one
        // place and both callers go through it.
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

        // The map marker of `location`, or of the nearest ancestor that has
        // one.
        //
        // A room does not usually carry a marker -- "Hall of Attainment"
        // has none, "College of Winterhold" does -- so the useful answer is
        // almost always a rung or two up the parentLoc chain. Without the
        // walk this resolves for almost nobody who lives indoors, which is
        // most people.
        bool MarkerFromLocation(RE::BGSLocation* location, RoadRoute::Origin& out, std::string& trail)
        {
            for (int depth = 0; location && depth < kMaxParentDepth; ++depth) {
                const char* edid = location->GetFormEditorID();
                trail += (depth == 0 ? "" : "->");
                trail += (edid && *edid) ? edid : "?";

                const auto markerPtr = location->worldLocMarker.get();
                if (auto* marker = markerPtr.get()) {
                    // A marker far from the player is in an unloaded cell,
                    // which is exactly when this matters -- so CellOf, not
                    // GetParentCell.
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
        OriginSource ResolveSenderOrigin(const MainThread::Token& mt, RE::Actor* sender, RoadRoute::Origin& out)
        {
            out = RoadRoute::ResolveOrigin(mt, sender);
            if (out.valid) {
                return OriginSource::Live;
            }

            auto* saveCell = CellOf(sender);
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
            if (saveCellInterior && MarkerFromLocation(saveCell->GetLocation(), out, cellTrail)) {
                logger::debug("VisitArrivalPoint: sender 0x{:08X} placed from their save cell's location ({})",
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
            if (MarkerFromLocation(home, out, homeTrail)) {
                logger::debug("VisitArrivalPoint: sender 0x{:08X} placed from the location they belong to ({})",
                              sender->GetFormID(),
                              homeTrail);
                return OriginSource::HomeLocation;
            }

            // Everything declined. Say what each rung actually saw, so the
            // next attempt does not have to be another run of the game.
            logger::warn("VisitArrivalPoint: sender 0x{:08X} could not be placed — save_cell={} interior={} "
                         "cell_location_trail='{}' editor_or_current_location={} location_trail='{}'",
                         sender->GetFormID(),
                         saveCell != nullptr,
                         saveCellInterior,
                         cellTrail,
                         home != nullptr,
                         homeTrail);
            return OriginSource::Unresolved;
        }

        struct Endpoints
        {
            bool resolved = false;
            RoadRoute::Origin sender;
            RoadRoute::Origin player;
            RE::NiPoint3 playerPos{};
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
            out.senderSource = ResolveSenderOrigin(mt, sender, out.sender);
            out.player = RoadRoute::ResolveOrigin(mt, player);
            out.playerPos = player->GetPosition();
            out.playerAngleZ = player->data.angle.z;
            out.resolved = out.sender.valid && out.player.valid;
            return out;
        });

        if (!ends.resolved) {
            logger::warn("VisitArrivalPoint: sender=0x{:08X} tier=none — origin unresolved "
                         "(sender_valid={} via={} player_valid={}). Nothing in the record or the save "
                         "could say where this visitor is or where they belong.",
                         senderId,
                         ends.sender.valid,
                         OriginSourceName(ends.senderSource),
                         ends.player.valid);
            return result;
        }
        if (ends.sender.worldSpace != ends.player.worldSpace) {
            logger::info("VisitArrivalPoint: sender=0x{:08X} tier=none — different worldspaces "
                         "(sender=0x{:08X} player=0x{:08X})",
                         senderId,
                         ends.sender.worldSpace,
                         ends.player.worldSpace);
            return result;
        }

        // Pure queries over both graphs — deliberately NOT inside a
        // main-thread hop.
        //
        // Route toward the corridor's way-in rather than toward the
        // sender directly. See CorridorTarget: routing at the sender
        // lets Route's frontier heuristic pick the branch behind the
        // player, and it did.
        RE::NiPoint3 corridor{};
        std::string corridorWhy;
        const bool haveCorridor =
            CorridorTarget(ends.player.worldSpace, ends.sender.position, ends.playerPos, corridor, corridorWhy);
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
                    plan.finePath, ends.playerPos, ends.playerAngleZ, minDist, maxDist, coverRadius, fineTally);
            });
            if (!kept.empty()) {
                tier = Tier::FineRoad;
            }
        }

        if (tier == Tier::None) {
            if (!cfg.visitArrivalAllowCoarseBearing) {
                logger::info("VisitArrivalPoint: sender=0x{:08X} no fine-road point and the bearing fallback "
                             "is switched off",
                             senderId);
            } else if (!plan.valid) {
                logger::info("VisitArrivalPoint: sender=0x{:08X} no route at all — neither graph could place "
                             "both ends",
                             senderId);
            } else {
                RE::NiPoint3 toward{};
                if (!BearingHome(plan.coarsePath, ends.playerPos, toward)) {
                    logger::info("VisitArrivalPoint: sender=0x{:08X} no usable bearing — none of the {} coarse "
                                 "node(s) on the route is {:.0f}u clear of the player",
                                 senderId,
                                 plan.coarsePath.size(),
                                 kBearingMinSeparationUnits);
                } else {
                    logger::info("VisitArrivalPoint: sender=0x{:08X} falling back to a bearing toward "
                                 "({:.0f},{:.0f}), {:.0f}u out",
                                 senderId,
                                 toward.x,
                                 toward.y,
                                 Dist2D(toward, ends.playerPos));
                    kept = MainThread::Run(pt, [&](const MainThread::Token&) {
                        return SampleBearingArc(
                            ends.playerPos, ends.playerAngleZ, toward, minDist, maxDist, coverRadius, bearingTally);
                    });
                    if (!kept.empty()) {
                        tier = Tier::CoarseBearing;
                    }
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
                         ends.playerPos.z,
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
            ToDegrees(std::atan2(ends.sender.position.y - ends.playerPos.y, ends.sender.position.x - ends.playerPos.x));
        const float pointBearing =
            ToDegrees(std::atan2(result.point.y - ends.playerPos.y, result.point.x - ends.playerPos.x));

        logger::info("VisitArrivalPoint: sender=0x{:08X} tier={} — ws=0x{:08X} home=({:.0f},{:.0f}) via={} "
                     "player=({:.0f},{:.0f},{:.0f}){} point=({:.0f},{:.0f},{:.0f}) dist={:.0f}u "
                     "bearing_home={:.0f}deg bearing_arrival={:.0f}deg within_fine={} "
                     "fine_nodes={} coarse_nodes={} fallbacks={} | fine[{}] | bearing[{}]",
                     senderId,
                     TierName(tier),
                     ends.player.worldSpace,
                     ends.sender.position.x,
                     ends.sender.position.y,
                     OriginSourceName(ends.senderSource),
                     ends.playerPos.x,
                     ends.playerPos.y,
                     ends.playerPos.z,
                     ends.player.viaLoadDoor ? " via-door" : "",
                     result.point.x,
                     result.point.y,
                     result.point.z,
                     Dist2D(result.point, ends.playerPos),
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
                          Dist2D(result.fallbacks[i], ends.playerPos));
        }
        return result;
    }
} // namespace NarrativeEngine::VisitArrivalPoint
