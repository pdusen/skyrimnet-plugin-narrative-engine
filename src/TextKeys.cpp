#include <TextKeys.h>

namespace NarrativeEngine::TextKeys
{
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
