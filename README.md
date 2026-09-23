# KFC Runtime

Windows x64 live Enshrouded provider, independent of the KFC asset parser.

Build with `cmake -S . -B build -A x64` and
`cmake --build build --config Release`. Install using
`cmake --install build --config Release --prefix <staging-directory>`.

The DLL and `config/runtime` belong next to the game executable.
Provider ABI 1 exports `KfcRuntimeAbi`, Initialize/Tick/Shutdown/Status and the
`ShroudforgeEcs*` component interface. Existing consumers must check ABI 1
before resolving operations. JSON profiles can change without rebuilding the
loader. Changes to native behavior require a new runtime DLL; incompatible
provider ABI changes also require a host update.

Profiles are selected by process target and image identity. An explicitly
opted-in profile can undergo structural revalidation for a changed image:
both hook signatures must match uniquely, overwritten bytes must match, parser
component sizes must agree, and live accesses validate identity and stride.
These checks are not proof that every engine semantic remains unchanged.
New hook calling conventions require native implementation work, not merely
editing offsets. The supplied profile derives from client build 1076226.

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
