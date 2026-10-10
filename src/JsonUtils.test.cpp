#include <JsonUtils.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>

// Unit tests for the defaulting JSON field readers.
//
// Pure functions over a JSON value, so no engine and nothing to mock. What they
// are actually for is surviving malformed input: every caller feeds these
// payloads that came back from an LLM or from SkyrimNet, where a field can be
// absent, null, or the wrong type entirely, and where nlohmann's own
// `value(key, def)` throws rather than falling back on a present-but-null key.
// The header records a real incident from that difference, so the wrong-type
// cases below are the point of the module rather than edge-case padding.

namespace
{
    using nlohmann::json;
    namespace JsonUtils = NarrativeEngine::JsonUtils;
} // namespace

TEST_CASE("JsonUtils::ClampParameterInt", "[JsonUtils]")
{
    // Happy path, re-run for every leaf below: a well-formed parameter object
    // whose key holds a number comfortably inside the range. Each case
    // overrides only the value, or the whole object where that is the point.
    //
    // The four numbers are deliberately all different. If the default equalled
    // a bound, or the in-range value equalled the default, half the cases below
    // would pass against the wrong answer.
    const std::string kKey = "count";
    constexpr int kDefault = 3;
    constexpr int kLow = 1;
    constexpr int kHigh = 10;
    json parameters = json::object({{kKey, 5}});

    SECTION("when the parameters are not an object")
    {
        // An array, not an object: what a caller gets when the model returns a
        // bare list where the schema asked for a parameter block.
        parameters = json::array({1, 2, 3});

        SECTION("should return the default")
        {
            REQUIRE(JsonUtils::ClampParameterInt(parameters, kKey, kDefault, kLow, kHigh) == kDefault);
        }
    }

    SECTION("when the key is absent")
    {
        parameters = json::object({{"somethingElse", 5}});

        SECTION("should return the default")
        {
            REQUIRE(JsonUtils::ClampParameterInt(parameters, kKey, kDefault, kLow, kHigh) == kDefault);
        }
    }

    SECTION("when the value is a string")
    {
        // A numeric string is still not a number. get<int>() would throw on it,
        // so the is_number() gate is what keeps the caller alive.
        parameters[kKey] = "5";

        SECTION("should return the default")
        {
            REQUIRE(JsonUtils::ClampParameterInt(parameters, kKey, kDefault, kLow, kHigh) == kDefault);
        }
    }

    SECTION("when the value is a boolean")
    {
        parameters[kKey] = true;

        SECTION("should return the default")
        {
            REQUIRE(JsonUtils::ClampParameterInt(parameters, kKey, kDefault, kLow, kHigh) == kDefault);
        }
    }

    SECTION("when the value is null")
    {
        parameters[kKey] = nullptr;

        SECTION("should return the default")
        {
            REQUIRE(JsonUtils::ClampParameterInt(parameters, kKey, kDefault, kLow, kHigh) == kDefault);
        }
    }

    SECTION("when the value is a number inside the range")
    {
        SECTION("should return that number")
        {
            REQUIRE(JsonUtils::ClampParameterInt(parameters, kKey, kDefault, kLow, kHigh) == 5);
        }
    }

    SECTION("when the value is below the range")
    {
        parameters[kKey] = -40;

        SECTION("should clamp up to the low bound")
        {
            REQUIRE(JsonUtils::ClampParameterInt(parameters, kKey, kDefault, kLow, kHigh) == kLow);
        }
    }

    SECTION("when the value is above the range")
    {
        parameters[kKey] = 9999;

        SECTION("should clamp down to the high bound")
        {
            REQUIRE(JsonUtils::ClampParameterInt(parameters, kKey, kDefault, kLow, kHigh) == kHigh);
        }
    }

    SECTION("when the value sits exactly on the low bound")
    {
        // std::clamp's bounds are inclusive; pinned so a rewrite to an exclusive
        // comparison cannot pass silently.
        parameters[kKey] = kLow;

        SECTION("should return the low bound")
        {
            REQUIRE(JsonUtils::ClampParameterInt(parameters, kKey, kDefault, kLow, kHigh) == kLow);
        }
    }

    SECTION("when the value sits exactly on the high bound")
    {
        parameters[kKey] = kHigh;

        SECTION("should return the high bound")
        {
            REQUIRE(JsonUtils::ClampParameterInt(parameters, kKey, kDefault, kLow, kHigh) == kHigh);
        }
    }

    SECTION("when the value is fractional")
    {
        // is_number() is true for floats too, so a model that writes 7.9 where an
        // integer was asked for is accepted and narrowed rather than rejected.
        parameters[kKey] = 7.9;

        SECTION("should narrow it to an int")
        {
            REQUIRE(JsonUtils::ClampParameterInt(parameters, kKey, kDefault, kLow, kHigh) == 7);
        }
    }

    SECTION("when the default itself is outside the range")
    {
        parameters = json::object({{"somethingElse", 5}});

        SECTION("should clamp the default too")
        {
            // The default goes through the same clamp as a real value, so an
            // out-of-range number cannot be smuggled in through the fallback.
            REQUIRE(JsonUtils::ClampParameterInt(parameters, kKey, 500, kLow, kHigh) == kHigh);
        }
    }
}

TEST_CASE("JsonUtils::StringOr", "[JsonUtils]")
{
    // Happy path: an object whose key holds an ordinary string. The default is
    // distinct from every value used below, so a case expecting the fallback
    // cannot pass on a coincidence.
    const std::string kKey = "name";
    const std::string kDefault = "(missing)";
    json obj = json::object({{kKey, "Ysolda"}});

    SECTION("when the value is not an object")
    {
        obj = json("just a string");

        SECTION("should return the default")
        {
            REQUIRE(JsonUtils::StringOr(obj, kKey, kDefault) == kDefault);
        }
    }

    SECTION("when the key is absent")
    {
        obj = json::object({{"somethingElse", "Ysolda"}});

        SECTION("should return the default")
        {
            REQUIRE(JsonUtils::StringOr(obj, kKey, kDefault) == kDefault);
        }
    }

    SECTION("when the key is present holding null")
    {
        // The case the module exists for. nlohmann's own value(key, def) falls
        // back only on an ABSENT key and throws type_error.302 on a present null
        // -- and SkyrimNet rows and LLM replies both emit explicit nulls as a
        // matter of course. One of them once threw out of an entire gossip tick.
        obj[kKey] = nullptr;

        SECTION("should return the default rather than throw")
        {
            REQUIRE(JsonUtils::StringOr(obj, kKey, kDefault) == kDefault);
        }
    }

    SECTION("when the value is a number")
    {
        obj[kKey] = 42;

        SECTION("should return the default")
        {
            REQUIRE(JsonUtils::StringOr(obj, kKey, kDefault) == kDefault);
        }
    }

    SECTION("when the value is a string")
    {
        SECTION("should return that string")
        {
            REQUIRE(JsonUtils::StringOr(obj, kKey, kDefault) == "Ysolda");
        }
    }

    SECTION("when the value is an empty string")
    {
        obj[kKey] = "";

        SECTION("should return the empty string rather than the default")
        {
            // Present-and-empty is a real answer, distinct from absent: a caller that
            // wrote an empty name meant an empty name.
            REQUIRE(JsonUtils::StringOr(obj, kKey, kDefault).empty());
        }
    }

    SECTION("when no default is given")
    {
        obj = json::object({{"somethingElse", "Ysolda"}});

        SECTION("should return an empty string")
        {
            REQUIRE(JsonUtils::StringOr(obj, kKey).empty());
        }
    }
}

TEST_CASE("JsonUtils::NumberOr", "[JsonUtils]")
{
    // Same shape as StringOr's happy path: an object whose key holds an
    // ordinary number, with a default distinct from every value used below so
    // a case expecting the fallback cannot pass on a coincidence.
    const std::string kKey = "gameTime";
    constexpr double kDefault = -1.0;
    json obj = json::object({{kKey, 4321.5}});

    SECTION("when the value is a number")
    {
        SECTION("should return it")
        {
            REQUIRE(JsonUtils::NumberOr(obj, kKey, kDefault) == 4321.5);
        }
    }

    SECTION("when the value is an integer")
    {
        obj[kKey] = 7;

        SECTION("should return it as a double")
        {
            REQUIRE(JsonUtils::NumberOr(obj, kKey, kDefault) == 7.0);
        }
    }

    SECTION("when the key is present holding null")
    {
        obj[kKey] = nullptr;

        SECTION("should return the default rather than throw")
        {
            // The case the helper exists for. A SkyrimNet event carrying
            // "gameTime":null threw out of the whole timeline merge.
            REQUIRE(JsonUtils::NumberOr(obj, kKey, kDefault) == kDefault);
        }
    }

    SECTION("when the value is a boolean")
    {
        obj[kKey] = true;

        SECTION("should return the default")
        {
            // nlohmann would happily convert a bool to 1.0; is_number() does
            // not, and a timestamp of "true" is not a timestamp.
            REQUIRE(JsonUtils::NumberOr(obj, kKey, kDefault) == kDefault);
        }
    }

    SECTION("when the value is a numeric string")
    {
        obj[kKey] = "4321.5";

        SECTION("should return the default")
        {
            REQUIRE(JsonUtils::NumberOr(obj, kKey, kDefault) == kDefault);
        }
    }

    SECTION("when the key is absent")
    {
        obj = json::object({{"somethingElse", 1.0}});

        SECTION("should return the default")
        {
            REQUIRE(JsonUtils::NumberOr(obj, kKey, kDefault) == kDefault);
        }
    }

    SECTION("when the value is not an object")
    {
        obj = json::array({1, 2, 3});

        SECTION("should return the default")
        {
            REQUIRE(JsonUtils::NumberOr(obj, kKey, kDefault) == kDefault);
        }
    }

    SECTION("when no default is given")
    {
        obj = json::object({{"somethingElse", 1.0}});

        SECTION("should return zero")
        {
            REQUIRE(JsonUtils::NumberOr(obj, kKey) == 0.0);
        }
    }
}
