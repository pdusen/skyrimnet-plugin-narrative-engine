#include <ThreadRole.h>

namespace NarrativeEngine
{
    namespace
    {
        // Every thread that never explicitly claims a role reads as
        // Foreign. That includes the game's own event-dispatch pool,
        // SkyrimNet's callback threads, and any DLL-boundary continuation
        // we don't own.
        thread_local ThreadRole g_role = ThreadRole::Foreign;
    } // namespace

    ThreadRole CurrentThreadRole() noexcept
    {
        return g_role;
    }

    ScopedThreadRole::ScopedThreadRole(ThreadRole newRole) noexcept : m_previous(g_role)
    {
        g_role = newRole;
    }

    ScopedThreadRole::~ScopedThreadRole() noexcept
    {
        g_role = m_previous;
    }
} // namespace NarrativeEngine
