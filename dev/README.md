# Development tools and profile workflow

This guide is for maintainers adding or reviewing support for an executable
build. These tools do not run when a player starts the game. The standalone
Runtime SDK and host integration are described in the [main README](../README.md)
and [SDK guide](../docs/SDK.md).

## Project layout

| Path | Purpose | Shipped to a modloader? |
| --- | --- | --- |
| `src/windows/` | Runtime implementation | Compiled into the DLL |
| `include/kfc_runtime/runtime.h` | Public C ABI for native modloader hosts | Yes, with the SDK package |
| `profiles/<game>/<target>/<build>.json` | Approved runtime profile for one executable build | Embedded in the DLL |
| `profiles/<game>/<target>/<target>-<build>.components.json` | Component map referenced by that profile | Embedded in the DLL |
| `sdk/runtime-api.json` | ABI/export inventory | Yes, with the SDK package |
| `dev/function-catalogs/<game>/<target>/<build>.json` | Function signatures and review evidence used during development | No; development package only |
| `dev/schemas/` | Schemas for profile, component, and function catalog data | No; development package only |
| `dev/tools/dev-console/` | Build selection, reflection import, profile generation, and approval | No; development package only |
| `dev/tools/enshrouded/` | Enshrouded-specific live inspectors, captures, and registry audit | No; development package only |
| `devdata/<build-id>/` | Local captures and unapproved drafts; ignored by Git | No |

Each build has one approved profile and its adjacent `.components.json` file.
Function catalogs are development evidence and are never loaded by the runtime.
New drafts stay in `devdata`; only reviewed, approved profile files are promoted
to `profiles/` and embedded by the runtime build.

## What the modloader receives

The **Runtime** package is what any native modloader consumes:

```text
bin/kfc-runtime.dll
lib/kfc-runtime.lib
include/kfc_runtime/runtime.h
share/kfc-runtime/sdk/runtime-api.json
licenses/nlohmann-json/LICENSE.MIT
```

The **Development** package contains the developer console, native inspectors,
approved profile sources, development catalogs, and schemas. It is not needed
by players or modloaders at game launch. Both packages carry the same project
version; the runtime ABI version is queried from `KfcRuntimeAbi()` and must
match before the host resolves other functions.

The C ABI currently exposes lifecycle/diagnostics, ECS describe/query/resolve/
read/guarded-write, profile-backed world operations, and named guarded patches.
It does not expose a Lua VM or automatically load arbitrary native plug-ins.
Different modloaders can integrate through the same public header and DLL ABI.

## Build and package

Use Windows x64 with CMake 3.24+ and MSVC:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
cmake --build build --config Release --target package
```

The generated ZIP packages are under `build/`. To stage only the runtime SDK:

```powershell
cmake --install build --config Release --component Runtime --prefix out\runtime-sdk
```

The modloader ships `out\runtime-sdk\bin\kfc-runtime.dll` with its game
integration and uses the installed public header as the ABI contract. The API
inventory is installed at `out\runtime-sdk\share\kfc-runtime\sdk\runtime-api.json`.
It can consume a versioned Runtime ZIP or pin this repository as a Git submodule
at a release commit. The modloader should not keep another copy of `src/windows/`.

## Runtime profiles

Profiles live only in `profiles/`, grouped by game, target, and game build. For
example, the current profile for client build 1076226 is:

```text
profiles/enshrouded/client/1076226.json
profiles/enshrouded/client/enshrouded-client-1076226.components.json
```

The profile contains hook signatures, structural offsets, named world
operations, and guarded patch definitions. Its `componentCatalog` field points
to the adjacent component map. CMake embeds the profile and sidecar in the DLL;
at runtime the provider verifies the executable identity and loads only the
matching approved profile. A new profile or component map requires rebuilding
the DLL so the matching embedded resources ship together.

PE timestamp and image size are recorded for identity. Newly generated profiles
also record executable SHA-256 and require an exact hash match. The shipped
1076226 profile predates SHA-256 provenance and uses timestamp plus image size.

## Developer workflow for a new build

The developer tools run only while adding support for a new game build. They do
not run at game launch:

```powershell
$exe = 'D:\Games\Enshrouded\enshrouded.exe'
$capture = 'devdata\enshrouded-client-new-build'
build\Release\kfc-runtime-dev.exe select-build $exe --out-dir $capture
build\Release\kfc-runtime-dev.exe extract-profile-functions `
  profiles\enshrouded\client\1076226.json --out "$capture\functions.json"
```

`select-build` records PE identity, compiler-described x64 function ranges, and
heuristic leads. It scans an existing function catalog only when the selected
executable matches that catalog's PE timestamp and image size. For a new build,
review/update the extracted development catalog's signatures, expected RVAs,
original bytes, calling conventions, and side effects, then scan it:

```powershell
build\Release\kfc-runtime-dev.exe scan-functions $exe `
  "$capture\functions.json" --out "$capture\function-scan.json"
```

Function ranges and byte matches do not infer source names or prove call
semantics. A developer must review the actual functions and calling conventions.

Start the same game executable, load into a world, then capture the live ECS
registry and join it against `kfc-parser`'s `reflection_data.json`:

```powershell
$game = Get-Process enshrouded | Select-Object -First 1
$reflection = '<path to kfc-parser reflection_data.json>'
build\Release\kfc-runtime-capture-ecs.exe $game.Id 2>&1 | Tee-Object "$capture\ecs-capture.log"
build\Release\kfc-runtime-dev.exe import-ecs-capture "$capture\ecs-capture.log" `
  --image-report "$capture\image-report.json" --reflection $reflection `
  --id enshrouded-client-new-build --out "$capture\components-live.json"
```

The importer reads the parser's native `version`/`types` format (`qualifiedName`
and `size`) and also accepts the normalized `entries` format. Parser `version`
identifies KFC data, not the game executable build; pass `--id` to name the game
build. Any unresolved name/size joins block profile approval.

Generate and validate a draft entirely under `devdata/`:

```powershell
$profileDraft = "$capture\new-build.json"
$componentsDraft = "$capture\enshrouded-client-new-build.components.json"
build\Release\kfc-runtime-dev.exe generate-profile `
  profiles\enshrouded\client\1076226.json `
  "$capture\image-report.json" "$capture\function-scan.json" `
  --components "$capture\components-live.json" `
  --catalog-out $componentsDraft --out $profileDraft
build\Release\kfc-runtime-dev.exe validate-profile $profileDraft
build\Release\kfc-runtime-dev.exe approve-profile $profileDraft `
  --function-scan "$capture\function-scan.json" --components $componentsDraft
```

Generation applies unique scanned hook signatures, shifted world-function RVAs,
and patch target/function ranges to the draft. It keeps structural offsets and
global data RVAs as review items because a binary signature scan cannot infer
their meaning. Approval checks exact executable identity, unique byte matches,
zero unresolved ECS joins, structural validity, and explicit developer review
of live layouts and function behavior. An unapproved draft is rejected by the
runtime.

After approval, place the pair under the build's production profile path and
build/package the DLL:

```powershell
$profileDir = 'profiles\enshrouded\client'
Copy-Item $profileDraft "$profileDir\new-build.json"
Copy-Item $componentsDraft "$profileDir\enshrouded-client-new-build.components.json"
cmake --build build --config Release --target kfc-runtime
cmake --build build --config Release --target package
```

The approved profile and component sidecar are the only per-build runtime data.
The function catalog, capture logs, reflection source, and draft stay in the
development area.

## Runtime behavior and limits

Profiles are selected by exact executable identity. If exact identity is absent,
structural revalidation is allowed only for a unique profile that explicitly
opts in and passes hook, byte, component-size, and live-access checks. These
checks do not prove that every engine semantic stayed the same; new function
semantics or calling conventions require profile review and sometimes native
implementation changes.

The provider retains hook trampolines and pins its DLL until process exit.
Shutdown stops new work and attempts to restore original code without freeing
addresses that may still be on engine thread stacks. Runtime binary updates take
effect after the game exits, not by hot-unloading. Timed-out writes may finish
after the caller times out, so hosts must reconcile state before retrying
non-idempotent operations.

## Enshrouded-specific inspection tools

The native inspection tools are kept in this repository because they gather
evidence for Enshrouded runtime profiles. They are developer-only tools, not
part of the public modloader API. Build them with the Development package or
individually with CMake:

```powershell
cmake --build build --config Release --target kfc-runtime-inspect-component-metadata
cmake --build build --config Release --target kfc-runtime-live-entity-manager-sample
cmake --build build --config Release --target kfc-runtime-live-entity-managers
cmake --build build --config Release --target kfc-runtime-live-type-references
cmake --build build --config Release --target kfc-runtime-audit-live-component-registry
```

These exploratory tools are read-only and their output is evidence to review,
not a compatibility verdict. For example, run the component registry audit
against a live world and an explicit type catalog:

```powershell
.\dev\tools\enshrouded\run-live-component-registry-audit.ps1 `
  -TypeCatalog '<path to runtime-ecs-types.json>'
```

The audit saves its raw log and candidate mappings under `devdata/`. It does not
edit a profile. Review candidate table matches and conflicts before changing a
development catalog or proposing a production profile entry.
