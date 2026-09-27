# KFC Runtime

KFC Runtime is a standalone Windows x64 library that gives native modloaders a
versioned interface to supported game builds. It owns game-version profiles,
profile validation, ECS access, guarded world operations, and guarded runtime
patches. It does not load mods or replace a modloader.

## What a modloader receives

Use the **Runtime** package produced by CMake. It contains:

- `bin/kfc-runtime.dll` — the runtime provider
- `lib/kfc-runtime.lib` — optional import library
- `include/kfc_runtime/runtime.h` — public C ABI
- `share/kfc-runtime/sdk/runtime-api.json` — machine-readable API inventory

A host can link to the import library or load the DLL and resolve the functions
from the header. Check the ABI before calling the API. The host owns loading,
calling `KfcRuntimeTick` from its game loop, and shutting the runtime down.
See [SDK integration](docs/SDK.md) for a minimal example and the full function
reference.

## Repository map

| Path | Purpose |
| --- | --- |
| `include/kfc_runtime/` | Stable public SDK header |
| `src/windows/` | Runtime implementation |
| `profiles/<game>/<target>/<build>.json` | Approved, runtime-loadable game profile |
| `sdk/` | ABI version and exported-function inventory |
| `dev/` | Profile-authoring tools, development catalogs, and schemas |
| `docs/` | SDK, profile, and architecture guides |

The Runtime package contains the provider and SDK. The separate **Development**
package contains profile sources and developer tools; modloaders do not need it.
See [Runtime and development responsibilities](docs/Architecture.md).

## Build

From a Visual Studio Developer PowerShell with CMake 3.24 or newer:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
cmake --build build --config Release --target package
```

The Runtime and Development ZIPs are written to `build/`. The current provider
supports Enshrouded client build `1076226`; other games and builds require an
approved profile and verified runtime support.

## Develop a profile

Profile creation is a maintainer workflow; it does not run when players start
the game. Follow [Development tools and profile workflow](dev/README.md). The
workflow inspects a selected executable, gathers live evidence, creates a
reviewable draft, validates it, and promotes only approved profile data.

## Versioning

The C ABI version is checked with `KfcRuntimeAbi`. ABI-breaking changes increase
the ABI and package major version. Game profiles are selected by executable
identity and are released with the provider that embeds them. Modloaders should
pin a KFC Runtime release or a specific repository commit.
