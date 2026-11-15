#include <RenderCheck.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>

// Unit tests for the render check's verdict on a reply, and for what it sends.

namespace
{
    using namespace NarrativeEngine::RenderCheck;

    const Sentinels kSentinels = Make(0x0123456789ABCDEFull);

    std::string Reply(bool file, bool decorator, bool loop, bool set)
    {
        std::string out;
        if (file) {
            out += "1: " + std::string{kFileSentinel} + "\n";
        }
        if (decorator) {
            out += "2: " + kSentinels.decorator + "\n";
        }
        out += "3: ";
        if (loop) {
            out += kSentinels.loop;
        }
        out += "+";
        if (set) {
            out += kSentinels.set;
        }
        return out;
    }
} // namespace

TEST_CASE("RenderCheck::Evaluate", "[RenderCheck]")
{
    SECTION("a reply repeating every sentinel passes all three")
    {
        const auto result = Evaluate(Reply(true, true, true, true), kSentinels);
        CHECK(result.file);
        CHECK(result.decorator);
        CHECK(result.include);
    }
    SECTION("the include passes only with both of its sentinels")
    {
        CHECK(Evaluate(Reply(true, true, true, true), kSentinels).include);
        CHECK_FALSE(Evaluate(Reply(true, true, true, false), kSentinels).include);
        CHECK_FALSE(Evaluate(Reply(true, true, false, true), kSentinels).include);
    }
    SECTION("each part fails on its own")
    {
        const auto noFile = Evaluate(Reply(false, true, true, true), kSentinels);
        CHECK_FALSE(noFile.file);
        CHECK(noFile.decorator);
        CHECK(noFile.include);
        const auto noDecorator = Evaluate(Reply(true, false, true, true), kSentinels);
        CHECK(noDecorator.file);
        CHECK_FALSE(noDecorator.decorator);
    }
    SECTION("an empty reply passes nothing")
    {
        const auto result = Evaluate("", kSentinels);
        CHECK_FALSE(result.file);
        CHECK_FALSE(result.decorator);
        CHECK_FALSE(result.include);
    }
    SECTION("another run's sentinels do not pass this one")
    {
        CHECK_FALSE(Evaluate(Reply(true, true, true, true), Make(42)).decorator);
    }
}

TEST_CASE("RenderCheck sends fresh sentinels, in the shape the template reads", "[RenderCheck]")
{
    const auto a = Make(1);
    const auto b = Make(0x0000000200020002ull);
    CHECK(a.decorator != b.decorator);
    CHECK(a.loop != b.loop);
    CHECK(a.set != b.set);

    const auto context = nlohmann::json::parse(Context(kSentinels));
    CHECK(context["render_check"]["file"] == "sentinel");
    CHECK(context["render_check"]["loop"] == nlohmann::json::array({kSentinels.loop}));
    CHECK(context["render_check"]["set"] == kSentinels.set);
    CHECK(nlohmann::json::parse(DecoratorRecord(kSentinels))["sentinel"] == kSentinels.decorator);
}
