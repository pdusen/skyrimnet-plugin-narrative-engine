#include <ConsoleCommand.h>

#include <logger.h>

namespace NarrativeEngine::ConsoleCommand
{
    bool Run(std::string_view command)
    {
        // Construct a transient Script form via the engine's form factory.
        // This mirrors what the console's input handler does when the
        // player types a command and hits Enter: build a Script form, set
        // its command text, compile + run it through the script VM, then
        // discard it. There is no quest / object association — the
        // compiled command runs in the console's global scope.
        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
        if (!factory) {
            logger::warn("ConsoleCommand::Run: no Script form factory available");
            return false;
        }

        auto* script = factory->Create();
        if (!script) {
            logger::warn("ConsoleCommand::Run: Script form factory returned null");
            return false;
        }

        script->SetCommand(command);

        // Target ref is null — equivalent to running the command with no
        // ref selected in the console (the most common case for
        // `startquest <id>`-style commands that operate on global forms).
        script->CompileAndRun(nullptr);

        // Script forms created via the form factory are stack-owned by
        // the caller of Create() in this transient pattern; they're not
        // registered into the form table and must be released here to
        // avoid leaking on every invocation.
        delete script;

        logger::info("ConsoleCommand::Run: executed '{}'", command);
        return true;
    }
} // namespace NarrativeEngine::ConsoleCommand
