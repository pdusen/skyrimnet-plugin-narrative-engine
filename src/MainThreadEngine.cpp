#include <MainThreadEngine.h>

#include <ThreadRole.h>

#include <RE/Skyrim.h>

#include <cassert>

namespace NarrativeEngine::MainThreadEngine
{
    namespace
    {
        // Belt-and-braces runtime check that the caller actually
        // obtained the token legitimately. Debug-only; in release the
        // wrappers trust the compile-time barrier.
        void AssertOnMainThread()
        {
            assert(CurrentThreadRole() == ThreadRole::Main
                   && "MainThreadEngine wrapper invoked from non-Main role — token forgery?");
        }

        Vec3 ToVec3(const RE::NiPoint3& p)
        {
            return Vec3{p.x, p.y, p.z};
        }
    } // namespace

    PlayerSnapshot ReadPlayerSnapshot(const MainThread::Token&)
    {
        AssertOnMainThread();

        PlayerSnapshot s;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            return s;
        }

        s.formID = pc->GetFormID();
        s.position = ToVec3(pc->GetPosition());

        if (auto* loc = pc->GetCurrentLocation()) {
            s.locationFormID = loc->GetFormID();
            if (const char* name = loc->GetFullName(); name && *name) {
                s.locationName = name;
            }
        }

        if (auto* cell = pc->GetParentCell()) {
            s.cellFormID = cell->GetFormID();
            s.cellIsInterior = cell->IsInteriorCell();
            if (const char* name = cell->GetFullName(); name && *name) {
                s.cellName = name;
            }
        }

        return s;
    }

    std::optional<ActorSnapshot> LookupActor(const MainThread::Token&, std::uint32_t formID)
    {
        AssertOnMainThread();

        auto* form = RE::TESForm::LookupByID(formID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor) {
            return std::nullopt;
        }

        const char* nameCStr = actor->GetDisplayFullName();
        if (!nameCStr || !*nameCStr) {
            return std::nullopt;
        }

        ActorSnapshot s;
        s.formID = actor->GetFormID();
        s.displayName = nameCStr;
        s.isDead = actor->IsDead();
        s.isDisabled = actor->IsDisabled();
        s.isInCombat = actor->IsInCombat();
        s.isPlayerTeammate = actor->IsPlayerTeammate();
        if (auto* state = actor->AsActorState()) {
            s.isBleedingOut = state->IsBleedingOut();
        }
        s.position = ToVec3(actor->GetPosition());
        return s;
    }

    std::optional<SkySnapshot> ReadCurrentSky(const MainThread::Token&)
    {
        AssertOnMainThread();

        auto* sky = RE::Sky::GetSingleton();
        if (!sky) {
            return std::nullopt;
        }

        SkySnapshot s;
        switch (sky->mode.get()) {
        case RE::Sky::Mode::kInterior:
            s.mode = SkyMode::Interior;
            break;
        case RE::Sky::Mode::kSkyDomeOnly:
            s.mode = SkyMode::SkyDomeOnly;
            break;
        case RE::Sky::Mode::kFull:
            s.mode = SkyMode::Full;
            break;
        default:
            // kNone / kTotal — treat as Interior (safest "not really
            // outdoors" default). Should never appear at runtime;
            // enumerated here for switch-completeness.
            s.mode = SkyMode::Interior;
            break;
        }

        if (auto* w = sky->currentWeather) {
            s.currentWeatherFormID = w->GetFormID();
            s.weatherFlags = static_cast<std::uint8_t>(w->data.flags.underlying());
            s.windSpeed = w->data.windSpeed;
            s.thunderLightningFrequency = w->data.thunderLightningFrequency;
        }

        return s;
    }
} // namespace NarrativeEngine::MainThreadEngine
