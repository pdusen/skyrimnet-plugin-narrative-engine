#include "FakeVTable.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace NarrativeEngine::Testing
{
    void UnimplementedVirtual()
    {
        std::fprintf(stderr,
                     "\n[EngineMock] FATAL: production code called an unimplemented virtual.\n"
                     "  A fabricated engine object took a virtual call whose vtable slot nothing\n"
                     "  filled in. Find the slot number in the CommonLibSSE header's trailing\n"
                     "  comment (// 00, // 01, ...) and register a stand-in with FakeObject::Slot.\n\n");
        std::fflush(stderr);
        std::abort();
    }

    FakeObject::FakeObject(std::size_t objectSize, std::size_t virtualSlots)
        : // At least a pointer's worth, since the vtable pointer itself lives at
          // offset 0 even for an interface with no data members.
          storage_(objectSize < sizeof(void*) ? sizeof(void*) : objectSize, std::byte{}),
          vtable_(virtualSlots, reinterpret_cast<void*>(&UnimplementedVirtual))
    {}

    void FakeObject::Slot(std::size_t index, void* fn)
    {
        if (index >= vtable_.size()) {
            std::fprintf(stderr,
                         "[EngineMock] FATAL: vtable slot %zu is past the %zu slots this object was "
                         "built with.\n",
                         index,
                         vtable_.size());
            std::fflush(stderr);
            std::abort();
        }
        vtable_[index] = fn;
    }

    void* FakeObject::Storage() noexcept
    {
        // Written on every call rather than once in the constructor, so a
        // FakeObject stays correct if its storage is ever memset by a test
        // reproducing a zero-initialised engine object.
        void* table = vtable_.data();
        std::memcpy(storage_.data(), &table, sizeof(table));
        return storage_.data();
    }
} // namespace NarrativeEngine::Testing
