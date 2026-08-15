#include <GossipLog.h>

#include <EventLogUtil.h>
#include <GossipGraph.h>
#include <logger.h>
#include <Settings.h>

#include <SKSE/SKSE.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <system_error>

namespace NarrativeEngine::GossipLog
{
    namespace
    {
        constexpr const char* kFileStem = "NarrativeEngine_Gossip";
        constexpr int kRotationSlots = 5;

        std::mutex g_mutex;
        std::ofstream g_file;
        std::uint32_t g_sessionCounter = 0;
        std::size_t g_linesWritten = 0;

        std::filesystem::path FilePathForSlot(int slot)
        {
            auto dir = SKSE::log::log_directory();
            if (!dir) {
                return {};
            }
            if (slot == 0) {
                return *dir / (std::string(kFileStem) + ".log");
            }
            return *dir / (std::string(kFileStem) + "." + std::to_string(slot) + ".log");
        }

        // Delete .5, shift .4 -> .5 ... current -> .1. Failures are
        // logged and swallowed: a rotation that cannot happen degrades
        // to "this session's file only", which is still useful.
        void RotateFilesLocked()
        {
            std::error_code ec;
            const auto oldest = FilePathForSlot(kRotationSlots);
            if (std::filesystem::exists(oldest, ec)) {
                std::filesystem::remove(oldest, ec);
                ec.clear();
            }
            for (int slot = kRotationSlots - 1; slot >= 1; --slot) {
                const auto src = FilePathForSlot(slot);
                const auto dst = FilePathForSlot(slot + 1);
                if (std::filesystem::exists(src, ec)) {
                    std::filesystem::rename(src, dst, ec);
                    ec.clear();
                }
            }
            const auto current = FilePathForSlot(0);
            if (std::filesystem::exists(current, ec)) {
                std::filesystem::rename(current, FilePathForSlot(1), ec);
                if (ec) {
                    logger::warn("GossipLog: rotate of '{}' failed: {}", current.string(), ec.message());
                }
            }
        }

        void WriteLineLocked(std::string_view body)
        {
            if (!g_file.is_open()) {
                return;
            }
            g_file << EventLogUtil::CurrentInGameTimestamp() << ' ' << body << '\n' << std::flush;
            ++g_linesWritten;
        }

        void Emit(std::string_view body)
        {
            std::scoped_lock lock(g_mutex);
            WriteLineLocked(body);
        }

        // Prefer the cached display name; fall back to the FormID so a
        // line is never silently anonymous.
        std::string NameOf(RE::FormID npc)
        {
            const auto& n = GossipGraph::NpcName(npc);
            return n.empty() ? std::format("0x{:08X}", npc) : n;
        }

        std::string LocOf(RE::FormID loc)
        {
            if (!loc) {
                return "-";
            }
            const auto& n = GossipGraph::LocationName(loc);
            return n.empty() ? std::format("0x{:08X}", loc) : n;
        }
    } // namespace

    void Initialize()
    {
        const auto& cfg = Settings::Get();
        logger::info("GossipLog: initialized (enabled={}, gossipEnabled={})", cfg.gossipLogEnabled, cfg.gossipEnabled);
    }

    void OnSessionStart()
    {
        const auto& cfg = Settings::Get();
        if (!cfg.gossipEnabled || !cfg.gossipLogEnabled) {
            return;
        }

        std::scoped_lock lock(g_mutex);
        if (g_file.is_open()) {
            g_file.flush();
            g_file.close();
        }
        RotateFilesLocked();
        ++g_sessionCounter;
        g_linesWritten = 0;

        const auto path = FilePathForSlot(0);
        g_file.open(path, std::ios::out | std::ios::trunc);
        if (!g_file.is_open()) {
            logger::error("GossipLog: failed to open '{}' for writing", path.string());
            return;
        }

        g_file << "# NarrativeEngine gossip trace (session " << g_sessionCounter << ")\n"
               << "# One line per event, absolute in-game timestamp first.\n"
               << "#   SEED    a rumor enters the world\n"
               << "#   TELL    a successful transmission; via= names the channel it travelled\n"
               << "#   WASTED  a telling that landed on someone who already knew (consumes quota)\n"
               << "#   RETIRE  a carrier stopped telling\n"
               << "#   BURNOUT the rumor's last carrier retired; carries the full summary\n"
               << "#   NOTE    seeder summaries, catch-up drains, census\n"
               << "# Hold crossings are flagged inline as XHOLD <from>-><to>.\n\n";
        g_file.flush();
        logger::info("GossipLog: opened '{}' for session {}", path.string(), g_sessionCounter);
    }

    void OnSessionEnd()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_file.is_open()) {
            return;
        }
        g_file << "\n# session " << g_sessionCounter << " ended, " << g_linesWritten << " lines\n";
        g_file.flush();
        g_file.close();
    }

    bool IsActive()
    {
        std::scoped_lock lock(g_mutex);
        return g_file.is_open();
    }

    void Seed(std::uint32_t rumorId,
              float notability,
              RE::FormID originNpc,
              RE::FormID settlement,
              std::int64_t sourceMemoryId)
    {
        Emit(std::format("SEED    r{:02}  notability={:.2f}  origin={:<24} @{}  memory={}",
                         rumorId,
                         notability,
                         NameOf(originNpc),
                         LocOf(settlement),
                         sourceMemoryId));
    }

    void Tell(std::uint32_t rumorId,
              std::uint32_t generation,
              float notability,
              RE::FormID from,
              RE::FormID to,
              std::string_view rung,
              RE::FormID location,
              RE::FormID fromHold,
              RE::FormID toHold)
    {
        std::string cross;
        if (fromHold != toHold) {
            cross = std::format("  XHOLD {}->{}", LocOf(fromHold), LocOf(toHold));
        }

        Emit(std::format("TELL    r{:02}  gen={}  n={:.2f}  {:<22} -> {:<22} rung={:<11} @{}{}",
                         rumorId,
                         generation,
                         notability,
                         NameOf(from),
                         NameOf(to),
                         rung,
                         LocOf(location),
                         cross));
    }

    void Wasted(std::uint32_t rumorId, RE::FormID from, RE::FormID to, int remaining)
    {
        Emit(std::format("WASTED  r{:02}  {:<22} -> {:<22} (already knows)  convs_left={}",
                         rumorId,
                         NameOf(from),
                         NameOf(to),
                         remaining));
    }

    void Retire(std::uint32_t rumorId, RE::FormID npc, std::string_view reason)
    {
        Emit(std::format("RETIRE  r{:02}  {:<22} reason={}", rumorId, NameOf(npc), reason));
    }

    void Burnout(std::uint32_t rumorId, const BurnoutStats& stats)
    {
        Emit(std::format("BURNOUT r{:02}  reach={}  depth={}  holds={}  settlements={}  days={:.1f}  "
                         "conversations={} ({} told, {} knew, {} missed, {} away, {} capped, "
                         "{} silent)",
                         rumorId,
                         stats.reach,
                         stats.depth,
                         stats.holds,
                         stats.settlements,
                         stats.days,
                         stats.conversations,
                         stats.transmissions,
                         stats.wasted,
                         stats.notCaught,
                         stats.unavailable,
                         stats.capped,
                         stats.silent));
    }

    void Harvest(const HarvestStats& stats)
    {
        Emit(std::format("HARVEST bucket={}/{}  actors={}  memories={}  candidates={}  sent={}  "
                         "(rejected: {} diary, {} no-content, {} claimed, {} same-event, "
                         "{} isolated, {} unfiltered)",
                         stats.bucket,
                         stats.bucketCount,
                         stats.bucketPopulation,
                         stats.memoriesExamined,
                         stats.candidates,
                         stats.sentForGeneration,
                         stats.rejectedDiary,
                         stats.rejectedNoContent,
                         stats.rejectedClaimed,
                         stats.rejectedSameEvent,
                         stats.rejectedIsolated,
                         stats.rejectedUnfiltered));
    }

    void Memory(std::int64_t memoryId, RE::FormID owner, float importance, std::string_view verdict)
    {
        Emit(std::format("MEMORY  m{:<8}  {:<22} imp={:.2f}  {}", memoryId, NameOf(owner), importance, verdict));
    }

    void Claim(std::int64_t memoryId, std::string_view action, std::size_t outstanding)
    {
        Emit(std::format("CLAIM   m{:<8}  {:<9} outstanding={}", memoryId, action, outstanding));
    }

    void Note(std::string_view text)
    {
        Emit(std::format("NOTE    {}", text));
    }
} // namespace NarrativeEngine::GossipLog
