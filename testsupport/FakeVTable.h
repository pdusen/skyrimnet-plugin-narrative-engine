#pragma once

#include <cstddef>
#include <vector>

// Fabricated engine objects that can take a virtual call.
//
// The mocked-engine harness hands most engine singletons back as opaque
// storage, because the mocked accessors answer out of EngineMock and never read
// through the pointer. That stops working the moment production code calls a
// VIRTUAL on one. A virtual call reads a vtable pointer from offset 0 and jumps
// through it, and zeroed storage means jumping through null.
//
// The gap is smaller than it looks. A vtable is just an array of function
// pointers, and CommonLibSSE's headers number every slot in a comment:
//
//     virtual ~IFormFactory();            // 00
//     virtual TESForm* CreateImpl() = 0;  // 01
//     virtual const char* GetFormName();  // 02
//
// So a working object is storage of the right size with a pointer to an array
// of our own functions at offset 0. That is what this builds.
//
// Conventions that matter on MSVC x64:
//
//   * A slot's function takes the object as its first parameter, exactly as a
//     free function standing in for a member does elsewhere in the harness.
//   * Slot 0 of a class with a virtual destructor is the "scalar deleting
//     destructor", called as `slot0(this, flags)` where bit 0 asks it to free
//     the storage. Ours never frees: the object belongs to the FakeObject that
//     made it, and letting engine code free harness memory would be worse than
//     leaking it for the length of a test.
//   * Unfilled slots abort by name rather than crashing, so production code
//     calling a virtual nobody anticipated says so.
//
// RTTI is not reproduced. MSVC keeps the complete-object locator at slot -1, so
// `dynamic_cast` or `typeid` on one of these would read off the front of the
// array. Nothing in this codebase does either to an engine object.
namespace NarrativeEngine::Testing
{
    class FakeObject
    {
    public:
        // `objectSize` is sizeof the engine type; `virtualSlots` is one past
        // the highest slot number the header comments give for it. Both are
        // read straight off the CommonLibSSE declaration.
        FakeObject(std::size_t objectSize, std::size_t virtualSlots);

        FakeObject(const FakeObject&) = delete;
        FakeObject& operator=(const FakeObject&) = delete;

        // Install a stand-in at one vtable slot. `fn` takes the object pointer
        // first, then the virtual's own parameters.
        void Slot(std::size_t index, void* fn);

        // The object. Stable for the lifetime of this FakeObject.
        void* Storage() noexcept;

        template <class T> T* As() noexcept
        {
            return static_cast<T*>(Storage());
        }

    private:
        std::vector<std::byte> storage_;
        std::vector<void*> vtable_;
    };

    // What an unfilled slot runs. Named so the abort can say which object and
    // slot were reached.
    [[noreturn]] void UnimplementedVirtual();
} // namespace NarrativeEngine::Testing
