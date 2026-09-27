# SDK integration

The public SDK is the C ABI in `include/kfc_runtime/runtime.h`. It is usable
from C or C++ and does not expose C++ implementation types. The Runtime ZIP
contains the header, DLL, import library, and `runtime-api.json`.

## Loading the provider

The host must check the ABI before using other exports. With dynamic loading on
Windows, the lifecycle is:

```cpp
#include <windows.h>
#include <kfc_runtime/runtime.h>

HMODULE provider = LoadLibraryW(L"kfc-runtime.dll");
if (!provider) return;

auto abi = reinterpret_cast<decltype(&KfcRuntimeAbi)>(
    GetProcAddress(provider, "KfcRuntimeAbi"));
auto initialize = reinterpret_cast<decltype(&KfcRuntimeInitialize)>(
    GetProcAddress(provider, "KfcRuntimeInitialize"));
auto tick = reinterpret_cast<decltype(&KfcRuntimeTick)>(
    GetProcAddress(provider, "KfcRuntimeTick"));
auto shutdown = reinterpret_cast<decltype(&KfcRuntimeShutdown)>(
    GetProcAddress(provider, "KfcRuntimeShutdown"));

if (!abi || !initialize || !tick || !shutdown || abi() != 5) {
    FreeLibrary(provider);
    return;
}
if (!initialize()) {
    FreeLibrary(provider);
    return;
}

// Call tick() from the host's game-loop integration while the provider is active.
// On unload, call shutdown() before FreeLibrary(provider).
```

Production code should resolve and check every export it uses. Do not call the
runtime after shutdown or unload. A modloader adapter may wrap this C ABI in its
own language-specific API.

## Function groups

| Group | Functions |
| --- | --- |
| Lifecycle and state | `KfcRuntimeAbi`, `KfcRuntimeInitialize`, `KfcRuntimeTick`, `KfcRuntimeShutdown`, `KfcRuntimeStatus`, `KfcRuntimeDiagnostics` |
| ECS | `KfcRuntimeEcsConfigure`, `KfcRuntimeEcsReady`, `KfcRuntimeEcsCanWrite`, `KfcRuntimeEcsDescribe`, `KfcRuntimeEcsQuery`, `KfcRuntimeEcsResolve`, `KfcRuntimeEcsRead`, `KfcRuntimeEcsWrite` |
| World | `KfcRuntimeWorldOperationAvailable`, `KfcRuntimeWorldContextActive`, `KfcRuntimeWorldEntityContextReady`, `KfcRuntimeWorldVoxelRead`, `KfcRuntimeWorldVoxelWrite`, `KfcRuntimeWorldEntitySpawn`, `KfcRuntimeWorldEntityPlace`, `KfcRuntimeWorldEntityDestroy`, `KfcRuntimeWorldEntityFinishBuilding` |
| Guarded patches | `KfcRuntimePatchAvailable`, `KfcRuntimePatchSetEnabled` |

The exact names and native signatures are listed in
[`runtime-api.json`](../sdk/runtime-api.json). ECS values use opaque byte
layouts described by the selected build profile. Hosts must use component sizes
reported by the runtime and must not assume one game's layouts apply to another.
`KfcRuntimeStatus` returns compact status text. `KfcRuntimeDiagnostics` returns
a bounded JSON snapshot with a `schemaVersion`; use it for optional host health
reporting, not as a profile-authoring or compatibility verdict.

World writes and runtime patches are profile-gated and validated by the
provider. A function returning `false` means the operation was unavailable,
invalid, or could not be safely completed; hosts should check the outcome and
runtime status before reporting success to users.

## ABI and package versions

`KfcRuntimeAbi()` returns `5`. A host must reject an unsupported ABI. ABI
breaking changes increment this number and the package major version. Compatible
additions are documented in the inventory and shipped with a new package
release. Pin a release or commit instead of tracking a moving branch.
