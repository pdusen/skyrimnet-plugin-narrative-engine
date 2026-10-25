#include <DebugNotify.h>

#include <AsyncDispatch.h>
#include <logger.h>
#include <MainThread.h>
#include <Settings.h>

#include <RE/M/Misc.h>

#include <array>
#include <format>
#include <utility>

namespace NarrativeEngine::DebugNotify
{
    void Post(std::string text)
    {
        if (text.empty()) {
            return;
        }
        // Checked here as well as on arrival: when the setting is off
        // this is the common case, and there is no reason to queue work
        // across two threads only to drop it.
        if (!Settings::Get().debugNotifications) {
            return;
        }

        AsyncDispatch::EnqueueWork([text = std::move(text)](const PluginThread::Token& pt) mutable {
            MainThread::FireAndForget(pt, [text = std::move(text)](const MainThread::Token&) {
                // Re-checked on arrival. A post queued a moment before
                // the dashboard toggle went off should not still appear.
                if (!Settings::Get().debugNotifications) {
                    return;
                }
                logger::debug("DebugNotify: {}", text);
                RE::DebugNotification(text.c_str());
            });
        });
    }

    void PostActorNamed(RE::FormID actor, std::function<std::string(const std::string& name)> compose)
    {
        if (!Settings::Get().debugNotifications) {
            return;
        }

        AsyncDispatch::EnqueueWork([actor, compose = std::move(compose)](const PluginThread::Token& pt) mutable {
            MainThread::FireAndForget(pt, [actor, compose = std::move(compose)](const MainThread::Token&) {
                if (!Settings::Get().debugNotifications) {
                    return;
                }
                const char* name = nullptr;
                if (auto* form = RE::TESForm::LookupByID(actor)) {
                    name = form->GetName();
                }
                const std::string text = compose((name && *name) ? std::string{name} : std::string{});
                logger::debug("DebugNotify: {}", text);
                RE::DebugNotification(text.c_str());
            });
        });
    }

    namespace Text
    {
        namespace
        {
            using TextKeys::Key;
            using TextKeys::Param;
            using TextTemplate::Kind;

            constexpr std::array kVisitorParams{Param{"visitor", Kind::Person}};
            constexpr std::array kRumorParams{Param{"origin", Kind::Person}};
            constexpr std::array kAmbushParams{Param{"group", Kind::Text}, Param{"attackers", Kind::Number}};
            constexpr Key kCourier{"Notify", "courier", {}};
            constexpr Key kVisitor{"Notify", "visitor", kVisitorParams};
            constexpr Key kRumor{"Notify", "rumor", kRumorParams};
            constexpr Key kAmbush{"Notify", "ambush", kAmbushParams};
            constexpr Key kUnknownVisitor{"Notify", "unknown.visitor", {}};
            constexpr Key kUnknownPerson{"Notify", "unknown.person", {}};
            constexpr Key kUnknownGroup{"Notify", "unknown.group", {}};

            std::string Or(const LanguageTable::Table& table, std::string_view value, const Key& fallback)
            {
                return value.empty() ? table.Render(fallback, {}) : std::string{value};
            }
        } // namespace

        const std::vector<TextKeys::Key>& Keys()
        {
            static const std::vector<Key> keys{
                kCourier, kVisitor, kRumor, kAmbush, kUnknownVisitor, kUnknownPerson, kUnknownGroup};
            return keys;
        }

        std::string CourierHasLetter(const LanguageTable::Table& table)
        {
            return table.Render(kCourier, {});
        }

        std::string VisitorComing(const LanguageTable::Table& table, std::string_view name)
        {
            return table.Render(
                kVisitor,
                TextTemplate::Arguments{}.Set("visitor", TextTemplate::Person{Or(table, name, kUnknownVisitor)}));
        }

        std::string RumorSpreading(const LanguageTable::Table& table, std::string_view origin)
        {
            return table.Render(
                kRumor,
                TextTemplate::Arguments{}.Set("origin", TextTemplate::Person{Or(table, origin, kUnknownPerson)}));
        }

        std::string AmbushSpawned(const LanguageTable::Table& table, std::string_view group, int attackers)
        {
            return table.Render(kAmbush,
                                TextTemplate::Arguments{}
                                    .Set("group", Or(table, group, kUnknownGroup))
                                    .Set("attackers", static_cast<std::int64_t>(attackers)));
        }
    } // namespace Text
} // namespace NarrativeEngine::DebugNotify
