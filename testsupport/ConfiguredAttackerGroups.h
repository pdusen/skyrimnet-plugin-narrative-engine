#pragma once

#include <AmbushAttackerGroups.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

// Writes the attacker-group file the module parses, loads it, and takes it away
// again afterwards.
//
// The groups are content rather than settings: they live in their own file
// under the mod's data directory, name game records by editor ID, and are meant
// to be edited or added to without touching the plugin. So a test that is about
// what the parser accepts writes the file it would be given, rather than
// building the parsed structure directly — the parse is most of what there is
// to get wrong.
//
// Only the file is removed on the way out; the directories stay, because other
// harness pieces write into the same tree and removing it under them would be a
// race for no benefit.
namespace NarrativeEngine::Testing
{
    class ConfiguredAttackerGroups
    {
    public:
        explicit ConfiguredAttackerGroups(const std::string& contents)
        {
            std::error_code ec;
            std::filesystem::create_directories(Directory(), ec);
            {
                std::ofstream out{Path(), std::ios::binary | std::ios::trunc};
                out << contents;
            }
            AmbushAttackerGroups::Load();
        }

        ~ConfiguredAttackerGroups()
        {
            std::error_code ec;
            std::filesystem::remove(Path(), ec);
            AmbushAttackerGroups::Load();
        }

        ConfiguredAttackerGroups(const ConfiguredAttackerGroups&) = delete;
        ConfiguredAttackerGroups& operator=(const ConfiguredAttackerGroups&) = delete;

    private:
        static std::filesystem::path Directory()
        {
            return "Data/SKSE/Plugins/NarrativeEngine";
        }

        static std::filesystem::path Path()
        {
            return Directory() / "AttackerGroups.ini";
        }
    };
} // namespace NarrativeEngine::Testing
