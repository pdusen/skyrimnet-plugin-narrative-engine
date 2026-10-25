# Reading SkyrimNet's internals from its DLL

## TL;DR

`SkyrimNet.dll` ships without a PDB, but it is unusually easy to read. Its logging macros embed the **fully
qualified name of the calling function** and the **source file path** as string literals, so most of its
interesting functions name themselves. With nothing but Python, `pefile` and `capstone` you can go from a log
message to the function that prints it, see what it calls, and reconstruct its logic. No Ghidra or IDA is needed.

Use this to **learn how SkyrimNet does something**, then reproduce that behaviour against public data (files on
disk, the engine) or ask upstream for an API. **Do not call SkyrimNet internals.** Nothing about them is stable:
addresses move with every beta, and a wrong guess at a calling convention crashes the player's game.

The findings so far: [skyrimnet-bio-template-name-assignment.md](skyrimnet-bio-template-name-assignment.md) and
[skyrimnet-character-profile-lookup.md](skyrimnet-character-profile-lookup.md).

## What is in the binary

Measured on Beta 26 RC4 (68 MB DLL, 26 MB of code):

- **About 2,750 distinct `SkyrimNet::Namespace::Class::Method` strings**, some of them lambdas
  (`…::<lambda_3>::operator ()`). Each is referenced from the function it names, because the logger is passed
  `__FUNCTION__`. Naming every function that references exactly such a string names about 3,700 functions.
- **Source paths** (`C:\actions-runner\_work\SkyrimNet\SkyrimNet\src\Content\ContentRegistry.cpp`), which give the
  module structure.
- **Log format strings**, which document intent (`Plugin '{}' v{} in '{}' is superseded by …`).
- **About 4,300 RTTI type names**, which give class names and vtables.
- **CommonLibSSE-NG relocation thunks.** An engine call is a lazy-initialised `REL::Relocation` whose SE / AE / VR
  ids are visible as immediates: `mov qword ptr [rsp+0x20], 0x4b9a` is id 19354. Look that id up in
  `include/RE/Offsets.h` in the CommonLibSSE-NG source in the vcpkg buildtree to name the engine function
  (`TESObjectREFR::GetDisplayFullName`). SkyrimNet's AE ids do not always match our fork's, so trust the SE id.

## The method

1. **Index the DLL.** Take function boundaries from `.pdata` (one `RUNTIME_FUNCTION` per function, about 71,000
   entries). Collect every `.rdata` string with its address. Collect every RIP-relative `lea` / `mov`
   (`48/4C 8D|8B` with ModRM `mod=00, rm=101`) and `call rel32` target. A byte-level regex over `.text` does all of
   this in seconds; there is no need for a full disassembly.
2. **Start from a string.** Find a log message or name near the behaviour you care about. Then find the functions
   that reference it.
3. **Disassemble that one function** with capstone, annotating each RIP-relative operand with the string it points
   at and each call with its auto-name. Most of the logic reads directly from the order of string references and
   calls. Read the instructions only for the small parts that matter (a mask, a format string's arguments, a
   comparison).
4. **Check against ground truth.** Never trust a decoded rule until it has reproduced real data.

Keep the scripts in the session scratchpad. They are throwaway by design, and they are fast to rewrite: the index
and query tool was about 150 lines.

## Ground truth to check against

- **`SkyrimNet/data/SkyrimNet-*.db`**, one SQLite database per save. `uuid_mappings` holds what SkyrimNet assigned
  each actor it registered (`form_id`, `actor_name`, `bio_template_name`, and in newer schemas `mod_file`,
  `local_form_id`). Open it read-only (`file:…?mode=ro`).
- **`SkyrimNet/logs/openrouter_input*.log`**: every prompt SkyrimNet sent, fully rendered. This is the way to test
  *which content* SkyrimNet used: find text that exists in only one candidate file and search for it.
- **`Documents/My Games/Skyrim Special Edition/SKSE/SkyrimNet.log`**: boot-time layer discovery, registry load and
  snapshot resolution, with source line numbers.

## Gotchas

- **Addresses are per build.** Re-index after every SkyrimNet update. Record findings as behaviour, not offsets.
- **An auto-name is a hint, not proof.** A small function inlined into a bigger one makes the bigger one reference
  the small one's name string, so one function can carry several names. Read before trusting.
- **Under MO2 the paths differ.** SkyrimNet logs its root as `Stock Game\Data\SKSE\Plugins\SkyrimNet`, but the real
  files are spread across `mods/<mod>/` and `overwrite/`. Look in both when matching log paths to disk.
