#pragma once

#include <Settings.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace NarrativeEngine::Testing
{
    // Puts an INI on disk, reloads Settings from it, and removes it again on
    // the way out.
    //
    // Settings is a process-wide singleton loaded from a fixed path, so a test
    // that wants a particular threshold has no way to inject one — it has to
    // write the file the module reads. Reloading in the destructor as well as
    // the constructor is what keeps cases independent: the next case sees
    // built-in defaults rather than whatever the last one configured.
    //
    // Only the file is removed. The directories are left alone: a real install
    // has them, and deleting them turns a later write into a failure that is a
    // property of the missing directory rather than of the module under test.
    struct ConfiguredSettings
    {
        explicit ConfiguredSettings(const std::string& body)
        {
            std::filesystem::create_directories(Path().parent_path());
            std::ofstream out{Path(), std::ios::binary | std::ios::trunc};
            out << body;
            out.close();
            Settings::Load();
        }

        ~ConfiguredSettings()
        {
            std::error_code ec;
            std::filesystem::remove(Path(), ec);
            Settings::Load();
        }

        ConfiguredSettings(const ConfiguredSettings&) = delete;
        ConfiguredSettings& operator=(const ConfiguredSettings&) = delete;

        static std::filesystem::path Path()
        {
            return "Data/SKSE/Plugins/NarrativeEngine.ini";
        }
    };
} // namespace NarrativeEngine::Testing
