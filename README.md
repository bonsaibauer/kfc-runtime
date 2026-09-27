# KFC Runtime

Windows x64 live Enshrouded provider, independent of the KFC asset parser.

Build with `cmake -S . -B build -A x64` and
`cmake --build build --config Release`. Install using
`cmake --install build --config Release --prefix <staging-directory>`.

The DLL belongs next to the game executable. Compatibility profiles from
`compatibility/profiles/` are embedded into the DLL during the CMake build.
Provider ABI 4 exports `KfcRuntimeAbi`, Initialize/Tick/Shutdown/Status, the
`ShroudforgeEcs*` component interface, bounded voxel read/write, and typed
profile-backed entity calls, and guarded build-profile runtime patches. Existing consumers must check ABI 4 before resolving
operations. A profile change requires rebuilding the runtime DLL. Changes to
native behavior require a new runtime DLL; incompatible provider ABI changes also
require a host update.

The world Lua surface exposes profile-backed voxel read/write and typed entity
spawn/place/destroy/finish calls. Spawn is dispatched from the prop update with
the live execution view; placement calls require the actor placement frame. A
spawn token confirms only that the native command was queued. Placement and
removal report native dispatch, not persistence or entity-manager materialization.

Profiles are selected for the running process by executable name and PE image
timestamp and size. A unique exact image match always takes priority. If there
is no exact match, structural revalidation is allowed only when exactly one
profile for that executable opts in. An opted-in profile must pass structural
checks for a changed image:
all dispatcher and world-context hook signatures must match uniquely, overwritten bytes must match, parser
component sizes must agree, and live accesses validate identity and stride.
These checks are not proof that every engine semantic remains unchanged.
New hook calling conventions require native implementation work, not merely
editing offsets. The supplied profile derives from client build 1076226.

KFC reflection entries describe data types and layouts; they do not name
executable functions or gameplay operations. Runtime patch IDs are stable
operation names resolved through build-specific code signatures. The runtime
accepts a patch only when its signature matches exactly once, and diagnostics
include the match count and target RVA so a known operation can be checked
against the installed executable.

The ECS diagnostic also records each live entity's template UUID/name alongside
that archetype's component indices, strides and offsets. These rows can be
joined to extracted `TemplateResource` assets by UUID; this supplies per-template
constraints for resolving same-size component types without relying on serialized
component order. The snapshot is diagnostic evidence, not an automatic index
assignment when multiple mappings still fit.

Hooks retain published trampoline memory and pin this DLL until process exit.
Shutdown stops admission and attempts to restore original code without
freeing return addresses that may still be on engine thread stacks. Runtime
binary updates therefore take effect after game exit, not by hot-unloading.

Timed-out queries/read operations retain their own storage until completion.
A write that has already started may complete after a timeout: consumers
must reconcile state before retrying non-idempotent operations.

Third-party dependency: nlohmann/json 3.12.0, MIT; see third_party/nlohmann.

Standalone inspection tools are maintained in ShroudForge under
`Shroudforge_Modules/runtime-diagnostics/native-tools`. This repository contains
the native provider, its public interface and build-specific compatibility data.
