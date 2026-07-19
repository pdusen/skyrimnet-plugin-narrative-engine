#include <WeatherEventLog.h>

#include <EventLogUtil.h>
#include <logger.h>
#include <PhaseTracker.h>
#include <Settings.h>
#include <SkyrimNetEvents.h>

#include <nlohmann/json.hpp>

#include <RE/Skyrim.h>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace NarrativeEngine::WeatherEventLog
{
    namespace
    {
        constexpr std::uint32_t kRecordVersion = 1;

        // Ring-buffer cap fallback if the setting is missing / non-positive.
        constexpr std::size_t kDefaultMaxStored = 128;

        // Priority-ordered primary category. When multiple flag bits are
        // set on a single weather (rare but not impossible), the highest
        // priority wins — snowy > rainy > pleasant > cloudy > other. This
        // ordering matches how the vanilla weather system prioritizes
        // narrative feel: precipitation is more load-bearing than the
        // pleasant/cloudy distinction.
        enum class Primary : std::uint8_t
        {
            Other = 0,
            Cloudy = 1,
            Pleasant = 2,
            Rainy = 3,
            Snowy = 4,
        };

        struct WeatherCategory
        {
            Primary primary = Primary::Other;
            bool stormy = false;

            bool operator==(const WeatherCategory& rhs) const
            {
                return primary == rhs.primary && stormy == rhs.stormy;
            }
            bool operator!=(const WeatherCategory& rhs) const
            {
                return !(*this == rhs);
            }
        };

        const char* PrimaryName(Primary p)
        {
            switch (p) {
            case Primary::Other:
                return "Other";
            case Primary::Cloudy:
                return "Cloudy";
            case Primary::Pleasant:
                return "Pleasant";
            case Primary::Rainy:
                return "Rainy";
            case Primary::Snowy:
                return "Snowy";
            }
            return "?";
        }

        struct InternalEvent
        {
            WeatherCategory from{};
            WeatherCategory to{};
            double localTime = 0.0; // Unix-epoch seconds
            double gameTime = 0.0;  // time-of-day seconds [0..86400)
        };

        std::mutex g_mutex;
        std::vector<InternalEvent> g_events;

        // Session-only pending queue drained by EventHistoryWriter.
        // Populated at emit time (right after PushLocked). Not
        // persisted — anything sitting here at save/load boundary
        // gets flushed to the outgoing session's history file by
        // EventHistoryWriter::OnSessionEnd and then discarded.
        std::vector<EventLogUtil::HistoryEntry> g_pendingHistory;

        // Sentinel unpopulated snapshot — indicates "no baseline yet";
        // the next Poll seeds instead of diffing.
        std::optional<WeatherCategory> g_lastCategory;

        // Unpaused-seconds accumulator toward the next sky sample.
        // Grows by the caller-supplied `unpausedElapsedSeconds` each
        // Poll invocation (Poll is only called when unpaused, so the
        // increments are all unpaused time). When it crosses the
        // configured interval, the sample fires and the interval is
        // subtracted (overshoot rolls into the next window rather than
        // being discarded). Session-only — not persisted.
        double g_unpausedSecondsSinceLastCheck = 0.0;

        // Unpaused-seconds accumulator toward the debounce clearing.
        // Same shape as g_unpausedSecondsSinceLastCheck but reset to
        // zero on emit rather than subtracted, since the debounce is a
        // one-shot "wait N unpaused seconds before I'll emit again"
        // rather than a periodic cadence.
        double g_unpausedSecondsSinceLastEmit = 0.0;

        std::size_t MaxStored()
        {
            const int v = Settings::Get().weatherEventsMaxStored;
            if (v <= 0)
                return kDefaultMaxStored;
            return static_cast<std::size_t>(v);
        }

        double PollIntervalSeconds()
        {
            const int v = Settings::Get().weatherEventPollIntervalSeconds;
            return v <= 0 ? 30.0 : static_cast<double>(v);
        }

        double DebounceSeconds()
        {
            const int v = Settings::Get().weatherEventsDebounceSeconds;
            return v < 0 ? 20.0 : static_cast<double>(v);
        }

        // Derive the category tuple from a TESWeather*. Null → Other/
        // non-stormy — treated as "no meaningful weather," a no-op diff
        // against a previous null (Sky::currentWeather can transiently
        // be null right after a save-load boundary).
        WeatherCategory DeriveCategory(RE::TESWeather* w)
        {
            WeatherCategory c;
            if (!w) {
                return c;
            }
            using Flag = RE::TESWeather::WeatherDataFlag;
            const auto flags = w->data.flags.get();
            const auto has = [flags](Flag f) {
                return (static_cast<std::uint8_t>(flags) & static_cast<std::uint8_t>(f)) != 0;
            };
            // Precipitation wins over pleasant/cloudy — a "cloudy rainy"
            // weather is still fundamentally rainy for narrative purposes.
            if (has(Flag::kSnow)) {
                c.primary = Primary::Snowy;
            } else if (has(Flag::kRainy)) {
                c.primary = Primary::Rainy;
            } else if (has(Flag::kPleasant)) {
                c.primary = Primary::Pleasant;
            } else if (has(Flag::kCloudy)) {
                c.primary = Primary::Cloudy;
            } else {
                c.primary = Primary::Other;
            }
            // Stormy = a precipitation weather with either high wind
            // (thunderstorm / blizzard) or authored lightning. Non-precip
            // weathers can't be classified stormy — a "stormy clear" would
            // render as just the transition to clear anyway.
            //
            // Empirically, vanilla SkyrimStormRain / SkyrimStormSnow have
            // `thunderLightningFrequency = -1` (raw byte 0xFF, the CK's
            // "unset" sentinel), so the thunder field alone doesn't
            // distinguish them from their non-storm counterparts. windSpeed
            // is the reliable discriminator — vanilla storm variants have
            // significantly higher windSpeed than their calm counterparts.
            // Threshold picked at 128 (midpoint of the uint8 range); tune
            // if in-game verification shows edge cases.
            if (c.primary == Primary::Rainy || c.primary == Primary::Snowy) {
                const bool highWind = w->data.windSpeed >= 128;
                const bool lightning = w->data.thunderLightningFrequency > 0;
                c.stormy = highWind || lightning;
            }
            return c;
        }

        // Category snapshot for the CURRENT sky state, main-thread. Null-
        // returns treated as "no reading available" — caller should not
        // diff on that cycle.
        std::optional<WeatherCategory> SampleCurrentCategory()
        {
            auto* sky = RE::Sky::GetSingleton();
            if (!sky) {
                if (Settings::Get().debugMode) {
                    logger::debug("WeatherEventLog: SampleCurrentCategory: Sky singleton null");
                }
                return std::nullopt;
            }
            // Only observe when the full outdoor sky is up. kInterior and
            // kSkyDomeOnly (dungeon entrances that keep a dome without
            // the full weather system) both skip.
            if (sky->mode.get() != RE::Sky::Mode::kFull) {
                if (Settings::Get().debugMode) {
                    logger::debug("WeatherEventLog: SampleCurrentCategory: Sky mode={} (not kFull); skip",
                                  static_cast<int>(sky->mode.get()));
                }
                return std::nullopt;
            }
            auto* wthr = sky->currentWeather;
            const WeatherCategory cat = DeriveCategory(wthr);
            if (Settings::Get().debugMode) {
                const auto formId = wthr ? wthr->GetFormID() : 0u;
                const auto rawFlags = wthr ? wthr->data.flags.underlying() : 0u;
                const auto lightning = wthr ? wthr->data.thunderLightningFrequency : 0;
                const auto wind = wthr ? wthr->data.windSpeed : 0u;
                logger::debug("WeatherEventLog: SampleCurrentCategory: weatherFormID=0x{:08X} rawFlags=0x{:02X} "
                              "wind={} thunder={} -> primary={} stormy={}",
                              formId,
                              rawFlags,
                              static_cast<unsigned>(wind),
                              static_cast<int>(lightning),
                              PrimaryName(cat.primary),
                              cat.stormy);
            }
            return cat;
        }

        void PushLocked(InternalEvent evt)
        {
            g_events.push_back(std::move(evt));
            while (g_events.size() > MaxStored()) {
                g_events.erase(g_events.begin());
            }
        }

        // Transition-to-sentence rendering. The table is deliberately
        // small and hand-authored — the LLM reads `text`, and a
        // deterministic sentence per transition is easier to reason
        // about than templating. Fallback: "The weather has shifted."
        //
        // Slug conventions: <primary>_<direction> where direction is
        // start / stop / intensify / subside. Slugs are stable and
        // exposed as `ne_kind` so any future consumer can branch on
        // kind without re-parsing the text.
        struct Rendering
        {
            std::string_view slug;
            std::string_view text;
        };

        // Classify the transition into (slug, sentence). Called with
        // guaranteed-non-equal categories.
        Rendering ClassifyTransition(WeatherCategory from, WeatherCategory to)
        {
            // Same primary category, stormy bit flipped: intensify /
            // subside.
            if (from.primary == to.primary) {
                if (!from.stormy && to.stormy) {
                    if (to.primary == Primary::Snowy) {
                        return {"blizzard_start", "A blizzard has struck."};
                    }
                    return {"storm_intensify", "The wind is picking up and the storm intensifies."};
                }
                if (from.stormy && !to.stormy) {
                    return {"storm_subside", "The storm is subsiding."};
                }
                return {"weather_shift", "The weather has shifted."};
            }

            // Precipitation transitions dominate the vocabulary — they
            // read as the most narratively concrete weather changes.
            if (to.primary == Primary::Rainy) {
                if (to.stormy) {
                    return {"storm_start", "A thunderstorm is rolling in."};
                }
                return {"rain_start", "Rain has started to fall."};
            }
            if (to.primary == Primary::Snowy) {
                if (to.stormy) {
                    return {"blizzard_start", "A blizzard has struck."};
                }
                return {"snow_start", "Snow is beginning to fall."};
            }
            if (from.primary == Primary::Rainy) {
                return {"rain_stop", "The rain has stopped."};
            }
            if (from.primary == Primary::Snowy) {
                return {"snow_stop", "The snow is letting up."};
            }

            // Non-precipitation transitions between clear/cloudy/other.
            if (to.primary == Primary::Pleasant) {
                return {"clear_start", "The clouds have parted and the sun is coming out."};
            }
            if (to.primary == Primary::Cloudy) {
                return {"cloudy_start", "The sky has clouded over."};
            }

            return {"weather_shift", "The weather has shifted."};
        }
    } // namespace

    void Initialize()
    {
        // No sinks — pure poll. Kept for symmetry with CombatEventLog.
        logger::info(
            "WeatherEventLog: initialized (poll interval={}s, debounce={}s)", PollIntervalSeconds(), DebounceSeconds());
    }

    void OnPostLoadGame()
    {
        auto sampled = SampleCurrentCategory();
        std::scoped_lock lock(g_mutex);
        g_lastCategory = sampled; // may be nullopt (interior on load — first exterior Poll seeds)
        // Reset the pause-aware accumulators — the first ~30s of unpaused
        // play after load will pass before the next sample fires. If the
        // sky truly hasn't changed by then, the sample matches the seeded
        // baseline and no event emits.
        g_unpausedSecondsSinceLastCheck = 0.0;
        g_unpausedSecondsSinceLastEmit = 0.0;
        if (sampled) {
            logger::info("WeatherEventLog: OnPostLoadGame: baseline seeded (primary={} stormy={})",
                         PrimaryName(sampled->primary),
                         sampled->stormy);
        } else {
            logger::info("WeatherEventLog: OnPostLoadGame: baseline unset (interior or no sky at load)");
        }
    }

    void OnPhaseAdvanced()
    {
        const double cutoff = PhaseTracker::PhaseEnteredAtRealTime();
        std::scoped_lock lock(g_mutex);
        if (cutoff <= 0.0) {
            // Baseline not stamped yet (pre-first-advance session start).
            // Nothing to prune against; clear defensively so a fresh
            // session doesn't carry ancient events through.
            g_events.clear();
            return;
        }
        std::erase_if(g_events, [cutoff](const InternalEvent& e) { return e.localTime < cutoff; });
    }

    void Poll(double unpausedElapsedSeconds)
    {
        std::scoped_lock lock(g_mutex);
        const bool debug = Settings::Get().debugMode;

        // Both accumulators only grow during unpaused play — Tick.cpp
        // gates on !paused before calling us, and passes the wall-clock
        // delta since the previous PollOnMainThread cycle. Paused cycles
        // never reach here, so paused time never accumulates.
        g_unpausedSecondsSinceLastCheck += unpausedElapsedSeconds;
        g_unpausedSecondsSinceLastEmit += unpausedElapsedSeconds;

        // Throttle gate — cheap early-return until the accumulator
        // crosses the configured interval.
        const double intervalSec = PollIntervalSeconds();
        if (g_unpausedSecondsSinceLastCheck < intervalSec) {
            return;
        }
        // Subtract rather than zero so any overshoot rolls into the next
        // window (matches how Tick handles its own tick interval —
        // accurate over long runs).
        g_unpausedSecondsSinceLastCheck -= intervalSec;
        if (debug) {
            logger::debug("WeatherEventLog: Poll: throttle cleared (crossed {:.1f}s of unpaused play)", intervalSec);
        }

        auto sampled = SampleCurrentCategory();
        if (!sampled) {
            // Interior / no sky. Leave g_lastCategory alone — when the
            // player exits back to exterior we'll diff against the last
            // exterior reading (which usually produces zero events since
            // weather rarely changed while indoors).
            return;
        }

        if (!g_lastCategory) {
            // No baseline yet (first Poll of the session, or first Poll
            // after post-load seeded a nullopt because we were indoors).
            g_lastCategory = sampled;
            if (debug) {
                logger::debug("WeatherEventLog: Poll: baseline seeded lazily (primary={} stormy={})",
                              PrimaryName(sampled->primary),
                              sampled->stormy);
            }
            return;
        }

        if (*sampled == *g_lastCategory) {
            if (debug) {
                logger::debug("WeatherEventLog: Poll: no change (primary={} stormy={})",
                              PrimaryName(sampled->primary),
                              sampled->stormy);
            }
            return; // no change
        }

        // Debounce — a rapid oscillation within a single window would
        // otherwise fire two events on consecutive samples. Measured in
        // unpaused seconds via the same accumulator model.
        const double debounceSec = DebounceSeconds();
        if (g_unpausedSecondsSinceLastEmit < debounceSec) {
            g_lastCategory = sampled; // still update baseline so we don't fire twice on the second flip
            if (debug) {
                logger::debug("WeatherEventLog: Poll: change detected but debounced ({:.1f}s of unpaused play "
                              "since last emit < {:.1f}s); baseline advanced to primary={} stormy={}",
                              g_unpausedSecondsSinceLastEmit,
                              debounceSec,
                              PrimaryName(sampled->primary),
                              sampled->stormy);
            }
            return;
        }

        InternalEvent e;
        e.from = *g_lastCategory;
        e.to = *sampled;
        e.localTime = EventLogUtil::NowUnixSeconds();
        e.gameTime = EventLogUtil::NowGameTimeSeconds();
        logger::info("WeatherEventLog: emit ({} stormy={}) -> ({} stormy={})",
                     PrimaryName(e.from.primary),
                     e.from.stormy,
                     PrimaryName(e.to.primary),
                     e.to.stormy);

        // History-writer feed: render the transition to its final
        // sentence NOW using the same table GetRenderedTail uses, so
        // the writer just concatenates without re-rendering. Gated
        // on the master switch so the queue can't grow unbounded on
        // long sessions with the writer off.
        if (Settings::Get().eventHistoryEnabled) {
            const auto r = ClassifyTransition(e.from, e.to);
            EventLogUtil::HistoryEntry h;
            h.localTime = e.localTime;
            h.inGameTimestamp = EventLogUtil::CurrentInGameTimestamp();
            h.sourceKind = std::string("internal/weather_event/") + std::string(r.slug);
            h.body = std::string(r.text);
            g_pendingHistory.push_back(std::move(h));
        }

        PushLocked(std::move(e));

        g_lastCategory = sampled;
        g_unpausedSecondsSinceLastEmit = 0.0;
    }

    nlohmann::json GetRenderedTail(double currentGameTimeSeconds)
    {
        std::vector<InternalEvent> snapshot;
        {
            std::scoped_lock lock(g_mutex);
            snapshot = g_events;
        }
        std::sort(snapshot.begin(), snapshot.end(), [](const InternalEvent& a, const InternalEvent& b) {
            return a.localTime < b.localTime;
        });

        nlohmann::json out = nlohmann::json::array();
        for (const auto& e : snapshot) {
            const auto r = ClassifyTransition(e.from, e.to);
            const double delta = currentGameTimeSeconds - e.gameTime;
            const std::string prefix = "[" + SkyrimNetEvents::FormatRelativeGameTime(delta < 0.0 ? 0.0 : delta) + "] ";

            nlohmann::json j;
            j["type"] = "weather_event";
            j["ne_kind"] = std::string(r.slug);
            j["localTime"] = e.localTime;
            j["gameTime"] = e.gameTime;
            j["originatingActorName"] = "";
            j["targetActorName"] = "";
            j["text"] = prefix + std::string(r.text);
            out.push_back(std::move(j));
        }
        return out;
    }

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (!intfc)
            return;
        // Prune before snapshotting so on-disk == in-memory rules —
        // same discipline CombatEventLog uses.
        OnPhaseAdvanced();

        std::vector<InternalEvent> snapshot;
        {
            std::scoped_lock lock(g_mutex);
            snapshot = g_events;
        }

        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("WeatherEventLog::OnSave: OpenRecord failed");
            return;
        }
        const auto count = static_cast<std::uint32_t>(snapshot.size());
        intfc->WriteRecordData(count);
        for (const auto& e : snapshot) {
            const std::uint8_t fromPrimary = static_cast<std::uint8_t>(e.from.primary);
            const std::uint8_t fromStormy = e.from.stormy ? 1 : 0;
            const std::uint8_t toPrimary = static_cast<std::uint8_t>(e.to.primary);
            const std::uint8_t toStormy = e.to.stormy ? 1 : 0;
            intfc->WriteRecordData(fromPrimary);
            intfc->WriteRecordData(fromStormy);
            intfc->WriteRecordData(toPrimary);
            intfc->WriteRecordData(toStormy);
            intfc->WriteRecordData(e.localTime);
            intfc->WriteRecordData(e.gameTime);
        }
    }

    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length)
    {
        if (!intfc)
            return;
        if (version != kRecordVersion) {
            logger::warn("WeatherEventLog::OnLoad: unknown version {} (length={}); clearing", version, length);
            OnRevert();
            return;
        }
        std::uint32_t count = 0;
        if (intfc->ReadRecordData(count) != sizeof(count)) {
            logger::error("WeatherEventLog::OnLoad: failed to read count");
            OnRevert();
            return;
        }
        std::vector<InternalEvent> loaded;
        loaded.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            InternalEvent e;
            std::uint8_t fromPrimary = 0;
            std::uint8_t fromStormy = 0;
            std::uint8_t toPrimary = 0;
            std::uint8_t toStormy = 0;
            if (intfc->ReadRecordData(fromPrimary) != sizeof(fromPrimary)
                || intfc->ReadRecordData(fromStormy) != sizeof(fromStormy)
                || intfc->ReadRecordData(toPrimary) != sizeof(toPrimary)
                || intfc->ReadRecordData(toStormy) != sizeof(toStormy)
                || intfc->ReadRecordData(e.localTime) != sizeof(e.localTime)
                || intfc->ReadRecordData(e.gameTime) != sizeof(e.gameTime)) {
                logger::error("WeatherEventLog::OnLoad: short read on record {}/{}", i, count);
                OnRevert();
                return;
            }
            if (fromPrimary > static_cast<std::uint8_t>(Primary::Snowy)
                || toPrimary > static_cast<std::uint8_t>(Primary::Snowy)) {
                logger::warn(
                    "WeatherEventLog::OnLoad: record {} invalid primary ({}, {}), skipping", i, fromPrimary, toPrimary);
                continue;
            }
            e.from.primary = static_cast<Primary>(fromPrimary);
            e.from.stormy = fromStormy != 0;
            e.to.primary = static_cast<Primary>(toPrimary);
            e.to.stormy = toStormy != 0;
            loaded.push_back(std::move(e));
        }
        {
            std::scoped_lock lock(g_mutex);
            g_events = std::move(loaded);
        }
        logger::info("WeatherEventLog::OnLoad: restored {} record(s)", count);
    }

    void OnRevert()
    {
        std::scoped_lock lock(g_mutex);
        g_events.clear();
        g_lastCategory.reset();
        g_unpausedSecondsSinceLastCheck = 0.0;
        g_unpausedSecondsSinceLastEmit = 0.0;
        g_pendingHistory.clear();
    }

    std::vector<EventLogUtil::HistoryEntry> DrainHistoryTail()
    {
        std::scoped_lock lock(g_mutex);
        return EventLogUtil::DrainVector(g_pendingHistory);
    }
} // namespace NarrativeEngine::WeatherEventLog
