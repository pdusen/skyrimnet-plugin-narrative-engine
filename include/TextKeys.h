#pragma once

#include <TextTemplate.h>

#include <span>
#include <string_view>
#include <vector>

// Every key a language file can hold, declared once.
//
// Code names the text it wants by a Key, never by a string literal, so a
// mistyped key is a compile error rather than a sentence that silently comes
// out empty. Each Key carries the placeholders its template may use, which is
// what lets the loader reject a translation that references one the code
// never supplies, before anyone reads the result.
//
// Declare a key beside the code that renders it:
//
//   inline constexpr std::array kCourierParams{TextKeys::Param{"sender", TextTemplate::Kind::Person}};
//   inline constexpr TextKeys::Key kCourier{"Notify", "courier", kCourierParams};
//
// and add it to the list All() returns, so the loader validates it and the
// shipped-files test requires English to define it.
namespace NarrativeEngine::TextKeys
{
    struct Param
    {
        std::string_view name;
        TextTemplate::Kind kind = TextTemplate::Kind::Text;
    };

    struct Key
    {
        // The INI section and key the text lives under, e.g. "Notify" and
        // "courier" for `[Notify] courier = ...`.
        std::string_view section;
        std::string_view name;
        std::span<const Param> params;
    };

    // Every key the plugin declares, from every subsystem.
    [[nodiscard]] const std::vector<Key>& All();

    [[nodiscard]] TextTemplate::Signature SignatureOf(const Key& key);
} // namespace NarrativeEngine::TextKeys
