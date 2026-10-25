#include <TextKeys.h>

namespace NarrativeEngine::TextKeys
{
    const std::vector<Key>& All()
    {
        // Each subsystem's keys join this list as its text moves into the
        // language files.
        static const std::vector<Key> keys;
        return keys;
    }

    TextTemplate::Signature SignatureOf(const Key& key)
    {
        TextTemplate::Signature out;
        out.reserve(key.params.size());
        for (const auto& param : key.params) {
            out.push_back({std::string{param.name}, param.kind});
        }
        return out;
    }
} // namespace NarrativeEngine::TextKeys
