#include "RelocationMocks.h"

#include <RE/Skyrim.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <string>
#include <unordered_map>

// The stand-ins themselves. See RelocationMocks.h for why they are reached
// through the address library rather than the linker.

namespace NarrativeEngine::Testing
{
    namespace
    {
        // ------------------------------------------------------------------
        // RE::BSFixedString
        // ------------------------------------------------------------------
        //
        // A BSFixedString is one pointer into the game's interned string pool,
        // aimed at the character data with a `BSStringPool::Entry` header
        // sitting immediately before it. Everything else about the class --
        // `c_str()`, `length()`, the copy constructor's `acquire()` -- is
        // inline header code that walks backwards from that pointer, so a
        // stand-in has to reproduce the shape, not just hand back a string.
        //
        // Interning IS reproduced, and has to be. The engine's pool hands back
        // the same entry for equal text, and code downstream depends on it:
        // BSTHashMap hashes a BSFixedString key by POINTER, because interning
        // makes pointer equality string equality. Without a pool,
        // TESForm::LookupByEditorID finds nothing a test registered, since two
        // BSFixedStrings built from one literal would hash apart.
        //
        // Pooled entries are never freed. The real pool keeps one for as long
        // as anything references it, and a test process is short enough that
        // outliving it beats refcounted teardown.
        std::unordered_map<std::string, char*>& StringPool()
        {
            static std::unordered_map<std::string, char*> pool;
            return pool;
        }

        RE::BSFixedString* FixedStringCtor8(RE::BSFixedString* self, const char* data)
        {
            if (!self)
                return self;

            const std::string text = data ? data : "";
            if (const auto it = StringPool().find(text); it != StringPool().end()) {
                *reinterpret_cast<const char**>(self) = it->second;
                return self;
            }

            const std::size_t length = text.size();
            auto* block = static_cast<std::byte*>(std::malloc(sizeof(RE::BSStringPool::Entry) + length + 1));
            if (!block) {
                std::fprintf(stderr, "[EngineMock] out of memory building a BSFixedString\n");
                std::abort();
            }

            auto* entry = reinterpret_cast<RE::BSStringPool::Entry*>(block);
            std::memset(static_cast<void*>(entry), 0, sizeof(RE::BSStringPool::Entry));
            // One reference, not wide. The refcount lives in the low bits of
            // _flags, which the inline acquire() increments.
            entry->_flags = 1;
            entry->_length = static_cast<std::uint32_t>(length);

            char* stored = reinterpret_cast<char*>(entry + 1);
            if (length > 0)
                std::memcpy(stored, text.data(), length);
            stored[length] = '\0';
            StringPool().emplace(text, stored);

            // `_data` is private and the class is standard-layout with that
            // pointer as its only member, so writing through the object's
            // address is how the real relocated constructor does it too.
            *reinterpret_cast<const char**>(self) = stored;
            return self;
        }

        void FixedStringRelease8(const char*& entry)
        {
            // Drops the caller's handle without touching the pooled block.
            // Freeing would be wrong now that entries are shared: another live
            // BSFixedString, or the next lookup of the same text, still points
            // at it.
            entry = nullptr;
        }

        // ------------------------------------------------------------------
        // RE::MemoryManager
        // ------------------------------------------------------------------
        //
        // Every engine type that uses TES_HEAP_REDEFINE_NEW routes `new` and
        // `delete` through Bethesda's allocator rather than the CRT's, so
        // `RE::MakeFunctionArguments` -- a plain `new FunctionArguments<...>`
        // -- cannot run without one. Backing it with the CRT heap is enough:
        // nothing under test inspects the allocator, only the memory.

        RE::MemoryManager* MemoryManagerGetSingleton()
        {
            alignas(16) static std::byte storage[sizeof(RE::MemoryManager)]{};
            return reinterpret_cast<RE::MemoryManager*>(storage);
        }

        void* MemoryManagerAllocate(RE::MemoryManager*, std::size_t size, std::int32_t alignment, bool aligned)
        {
            void* mem = aligned ? _aligned_malloc(size, alignment > 0 ? static_cast<std::size_t>(alignment) : 16u)
                                : std::malloc(size);
            // Zeroed, which the CRT heap does not promise and the game's
            // allocator effectively does for a fresh block. BSTScatterTable
            // grows by allocating an entry array and then treating an
            // all-zero entry as empty, so uninitialised bytes make a rehash
            // read a stale entry as live -- a crash that depends on what was
            // last on the heap and so appears at random.
            if (mem)
                std::memset(mem, 0, size);
            return mem;
        }

        void MemoryManagerDeallocate(RE::MemoryManager*, void* mem, bool aligned)
        {
            // The allocating call's `aligned` flag decides which heap the block
            // came from, and the engine passes the same flag back here.
            if (aligned) {
                _aligned_free(mem);
            } else {
                std::free(mem);
            }
        }

        void* MemoryManagerReallocate(RE::MemoryManager*,
                                      void* oldMem,
                                      std::size_t newSize,
                                      std::int32_t alignment,
                                      bool aligned)
        {
            const std::size_t align = alignment > 0 ? static_cast<std::size_t>(alignment) : 16u;
            const std::size_t oldSize = oldMem ? (aligned ? _aligned_msize(oldMem, align, 0) : _msize(oldMem)) : 0u;
            void* mem = aligned ? _aligned_realloc(oldMem, newSize, align) : std::realloc(oldMem, newSize);
            // Zero the grown tail, for the same reason Allocate zeroes:
            // BSTScatterTable grows its entry array and then reads the new
            // slots expecting them to be empty. Uninitialised bytes there make
            // a rehash — and the destructor's later walk — treat heap litter as
            // a live entry, which crashes at whatever address it litters with.
            if (mem && newSize > oldSize)
                std::memset(static_cast<std::byte*>(mem) + oldSize, 0, newSize - oldSize);
            return mem;
        }

        // ------------------------------------------------------------------
        // The engine's form tables
        // ------------------------------------------------------------------
        //
        // Not functions: `TESForm::LookupByID` and `LookupByEditorID` reach
        // their tables through DATA relocations, and REL::Relocation resolves
        // those the same way. The registered address is the address of a
        // pointer variable we own, so the lookup walks a real map a test filled
        // in.

        // Both tables are deliberately never destroyed, for the same reason
        // the string pool is not. A BSTHashMap frees its bucket array through
        // the mocked MemoryManager, which it reaches through a relocation --
        // and at static-destruction time there is no guaranteed order between
        // these tables and the relocation machinery they depend on. Getting
        // that order wrong is a crash AFTER the last assertion has already
        // passed, which reads as a flaky test rather than as what it is. A test
        // process is short enough that leaking two maps beats ordering them.
        RE::BSTHashMap<RE::FormID, RE::TESForm*>& FormTableStorage()
        {
            static auto* table = new RE::BSTHashMap<RE::FormID, RE::TESForm*>();
            return *table;
        }

        // What the relocation actually points at: a pointer TO the table.
        RE::BSTHashMap<RE::FormID, RE::TESForm*>* g_formTable = &FormTableStorage();
        RE::BSReadWriteLock g_formTableLock;

        RE::BSTHashMap<RE::BSFixedString, RE::TESForm*>& EditorIDTableStorage()
        {
            static auto* table = new RE::BSTHashMap<RE::BSFixedString, RE::TESForm*>();
            return *table;
        }

        RE::BSTHashMap<RE::BSFixedString, RE::TESForm*>* g_editorIDTable = &EditorIDTableStorage();
        RE::BSReadWriteLock g_editorIDTableLock;

        // ------------------------------------------------------------------
        // The table
        // ------------------------------------------------------------------
        //
        // Both ids per entry: `RELOCATION_ID(se, ae)` picks by the runtime
        // EngineMock claims, so registering only one would work until a test
        // asked for a different runtime.
        template <class Fn> constexpr std::uintptr_t Addr(Fn fn)
        {
            return reinterpret_cast<std::uintptr_t>(fn);
        }

        RE::BGSRelationship* GetRelationshipImpl(RE::TESNPC* a, RE::TESNPC* b)
        {
            return RelationshipBetween(a, b);
        }

        const std::array<RelocationMock, 22>& Table()
        {
            static const std::array<RelocationMock, 22> table = {{
                // BSFixedString::ctor8 — SE 67819, AE 69161
                {67819u, Addr(&FixedStringCtor8)},
                {69161u, Addr(&FixedStringCtor8)},
                // BSStringPool::Entry::release8 — SE 67847, AE 69192
                {67847u, Addr(&FixedStringRelease8)},
                {69192u, Addr(&FixedStringRelease8)},
                // MemoryManager::GetSingleton — SE 11045, AE 11141
                {11045u, Addr(&MemoryManagerGetSingleton)},
                {11141u, Addr(&MemoryManagerGetSingleton)},
                // MemoryManager::Allocate — SE 66859, AE 68115
                {66859u, Addr(&MemoryManagerAllocate)},
                {68115u, Addr(&MemoryManagerAllocate)},
                // MemoryManager::Deallocate — SE 66861, AE 68117
                {66861u, Addr(&MemoryManagerDeallocate)},
                {68117u, Addr(&MemoryManagerDeallocate)},
                // MemoryManager::Reallocate — SE 66860, AE 68116
                {66860u, Addr(&MemoryManagerReallocate)},
                {68116u, Addr(&MemoryManagerReallocate)},
                // TESForm::GetAllForms table pointer — SE 514351, AE 400507
                {514351u, reinterpret_cast<std::uintptr_t>(&g_formTable)},
                {400507u, reinterpret_cast<std::uintptr_t>(&g_formTable)},
                // ...and its lock — SE 514360, AE 400517
                {514360u, reinterpret_cast<std::uintptr_t>(&g_formTableLock)},
                {400517u, reinterpret_cast<std::uintptr_t>(&g_formTableLock)},
                // TESForm::GetAllFormsByEditorID table pointer — SE 514352, AE 400509
                {514352u, reinterpret_cast<std::uintptr_t>(&g_editorIDTable)},
                {400509u, reinterpret_cast<std::uintptr_t>(&g_editorIDTable)},
                // ...and its lock — SE 514361, AE 400518
                {514361u, reinterpret_cast<std::uintptr_t>(&g_editorIDTableLock)},
                {400518u, reinterpret_cast<std::uintptr_t>(&g_editorIDTableLock)},
                // BGSRelationship::GetRelationship — SE 23632, AE 24084
                {23632u, Addr(&GetRelationshipImpl)},
                {24084u, Addr(&GetRelationshipImpl)},
            }};
            return table;
        }
    } // namespace

    std::span<const RelocationMock> RelocationMockTable()
    {
        return Table();
    }

    RE::BSTHashMap<RE::FormID, RE::TESForm*>& FormTable()
    {
        return FormTableStorage();
    }

    RE::BSTHashMap<RE::BSFixedString, RE::TESForm*>& EditorIDTable()
    {
        return EditorIDTableStorage();
    }

    void UnregisteredRelocation()
    {
        std::fprintf(stderr,
                     "\n[EngineMock] FATAL: an unregistered relocation was called.\n"
                     "  Inline CommonLibSSE code resolved an address-library id that nothing in\n"
                     "  testsupport/RelocationMocks.cpp stands in for. Add the function and both\n"
                     "  of its RELOCATION_ID values to the table there.\n\n");
        std::fflush(stderr);
        std::abort();
    }
} // namespace NarrativeEngine::Testing
