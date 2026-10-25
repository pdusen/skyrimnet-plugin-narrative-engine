// TextKeys::All(): every key the plugin declares, gathered from each
// subsystem that renders language-file text. Kept out of the core library
// because some of these lists are generated from engine-coupled tables.
#include <TextKeys.h>

#include <PlotText.h>

namespace NarrativeEngine::TextKeys
{
    const std::vector<Key>& All()
    {
        static const std::vector<Key> keys = [] {
            std::vector<Key> out;
            const auto add = [&out](const std::vector<Key>& more) { out.insert(out.end(), more.begin(), more.end()); };
            add(PlotText::Keys());
            return out;
        }();
        return keys;
    }
} // namespace NarrativeEngine::TextKeys
