#include <CharacterBios.h>

#include <LLMTextSanitizer.h>

namespace NarrativeEngine::CharacterBios
{
    namespace
    {
        // Pull one inja block's text out of a bio file.
        //
        // A scanner rather than a template parser: these files are
        // generated to a fixed shape, the blocks do not nest, and the
        // text is either headed in a prompt or reduced to a bag of
        // words. Neither needs the template semantics.
        [[nodiscard]] std::string Block(std::string_view file, std::string_view name)
        {
            const std::string opener{"{% block " + std::string{name} + " %}"};
            const auto start = file.find(opener);
            if (start == std::string_view::npos) {
                return {};
            }
            const auto from = start + opener.size();
            const auto end = file.find("{% endblock %}", from);
            if (end == std::string_view::npos) {
                return {};
            }
            // Sanitize trims as well.
            return LLMTextSanitizer::Sanitize(file.substr(from, end - from));
        }
    } // namespace

    bool Bio::Empty() const noexcept
    {
        return summary.empty() && background.empty() && personality.empty() && aspirations.empty()
               && relationships.empty() && occupation.empty() && skills.empty();
    }

    Bio ParseBio(std::string_view file)
    {
        Bio bio;
        bio.summary = Block(file, "summary");
        bio.background = Block(file, "background");
        bio.personality = Block(file, "personality");
        bio.aspirations = Block(file, "aspirations");
        bio.relationships = Block(file, "relationships");
        bio.occupation = Block(file, "occupation");
        bio.skills = Block(file, "skills");
        return bio;
    }
} // namespace NarrativeEngine::CharacterBios
