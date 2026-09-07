#pragma once

#include <cstdint>
#include <type_traits>

// ICosaveIO — the narrow port through which persistent subsystems reach the
// SKSE co-save.
//
// Why this exists: co-save persistence is the one kind of engine coupling that
// can't be refactored away into a pure function. A filter over engine data can
// always be reduced to "gather a snapshot, decide from plain values", and the
// decision half tests fine on its own. Serialization can't: the behaviour worth
// testing IS the conversation with SKSE — how many bytes came back, what
// happens when the record is truncated mid-entry, and what becomes of a FormID
// whose owning plugin has left the load order. Snapshotting that away would
// leave nothing behind.
//
// So instead of removing the dependency we invert it. This interface names the
// three operations our tables actually use out of `SKSE::SerializationInterface`
// — write bytes, read bytes, remap a FormID across load orders. Production
// binds it to the real thing via `SKSECosaveIO` (see SKSECosaveIO.h, which is
// DLL-side and does include SKSE); tests bind it to an in-memory fake and can
// then reproduce a torn save, a removed mod, or a version mismatch exactly, on
// demand, with no game running.
//
// The interface is deliberately byte-level rather than value-level. It mirrors
// `WriteRecordData` / `ReadRecordData` / `ResolveFormID` one-for-one so the
// production adapter is a three-line transcription with no logic in it — there
// is nothing in the adapter for a test to be missing, which is what makes it
// honest to leave it untested.
//
// Record framing (`OpenRecord`, `GetNextRecordInfo`) is NOT part of this port.
// That is the caller's business: a beat opens its own record, then hands the
// stream to each table in turn. Keeping framing out means a table can't
// accidentally reach past its own payload.
namespace NarrativeEngine
{
    class ICosaveIO
    {
    public:
        virtual ~ICosaveIO() = default;

        ICosaveIO() = default;
        ICosaveIO(const ICosaveIO&) = delete;
        ICosaveIO& operator=(const ICosaveIO&) = delete;

        // Append `length` bytes to the currently-open record. Returns false if
        // the write did not land in full.
        virtual bool WriteBytes(const void* data, std::uint32_t length) = 0;

        // Read up to `length` bytes from the currently-open record into `out`.
        // Returns the number of bytes actually read, which is short at the end
        // of a record and zero past it — the caller compares against what it
        // asked for. Mirrors `SKSE::SerializationInterface::ReadRecordData`.
        virtual std::uint32_t ReadBytes(void* out, std::uint32_t length) = 0;

        // Translate a FormID recorded in an older save into the one that form
        // holds in the current load order. Returns false when the form no
        // longer resolves — usually because the plugin that owned it has been
        // removed — in which case the caller must drop the entry rather than
        // keep a FormID that now points at something else entirely.
        virtual bool ResolveFormID(std::uint32_t oldFormID, std::uint32_t& newFormID) const = 0;

        // Typed conveniences over the byte calls, so callers read as
        // `io.Write(count)` rather than repeating the address-and-size dance at
        // every field. Restricted to trivially-copyable types: a co-save record
        // is raw bytes, and blitting anything with a constructor, a vtable, or
        // an owning pointer into it would write an address that means nothing
        // on reload.
        template <class T> bool Write(const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "co-save fields are raw bytes; T must be trivially copyable");
            return WriteBytes(&value, static_cast<std::uint32_t>(sizeof(T)));
        }

        // Returns true only on a full-width read, so a truncated record fails
        // at the field that was cut rather than silently yielding a
        // half-populated value.
        template <class T> bool Read(T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "co-save fields are raw bytes; T must be trivially copyable");
            return ReadBytes(&value, static_cast<std::uint32_t>(sizeof(T))) == sizeof(T);
        }
    };
} // namespace NarrativeEngine
