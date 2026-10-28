#include <EvaluationPipeline.h>

#include <string>

// StripMarkdownFences, alone, in a translation unit that touches no
// engine and no logger.
//
// Split out of EvaluationPipeline.cpp rather than left there because it
// is the one function in that file every LLM-reading feature needs, and
// the rest of the file needs the plugin's SKSE plumbing. A probe driving
// a response parser had to link the whole pipeline to reach three dozen
// lines of string handling, and got a wall of `C2653: 'logger': is not a
// class or namespace name` for it -- the same trap Step 3 hit with the
// co-save reader, and resolved the same way.
namespace NarrativeEngine::EvaluationPipeline
{
    std::string StripMarkdownFences(const std::string& input)
    {
        // Trim leading/trailing whitespace. If the result begins with ```
        // (optionally followed by a language tag and newline), skip past
        // that opening fence and strip the closing fence too.
        auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        std::size_t start = 0;
        std::size_t end = input.size();
        while (start < end && isSpace(input[start]))
            ++start;
        while (end > start && isSpace(input[end - 1]))
            --end;
        if (start == end) {
            return {};
        }

        std::string trimmed = input.substr(start, end - start);

        // The fence does not have to be first. Models prepend "Here is
        // the JSON you asked for:" often enough that requiring the
        // response to BEGIN with the fence threw away otherwise perfect
        // objects -- the plot-birth fixtures caught it. Anything before
        // the first fence is preamble and is dropped.
        //
        // A response with no fence at all is returned untouched, which
        // is the common case and the one this must not disturb.
        const std::size_t fence = trimmed.find("```");
        if (fence == std::string::npos) {
            return trimmed;
        }
        trimmed.erase(0, fence);
        if (trimmed.size() < 6) {
            return trimmed;
        }

        const std::size_t firstNewline = trimmed.find('\n');
        if (firstNewline == std::string::npos) {
            return trimmed;
        }
        std::string body = trimmed.substr(firstNewline + 1);

        const std::size_t closing = body.rfind("```");
        if (closing != std::string::npos) {
            body = body.substr(0, closing);
        }

        std::size_t bodyEnd = body.size();
        while (bodyEnd > 0 && isSpace(body[bodyEnd - 1]))
            --bodyEnd;
        body.resize(bodyEnd);
        return body;
    }
} // namespace NarrativeEngine::EvaluationPipeline
