# Runtime and development responsibilities

KFC Runtime has two deliverables with different users.

## Runtime package

The Runtime package is loaded by a native modloader. It includes the provider
DLL, public header, import library, and API inventory. The provider validates
the executable identity, loads its embedded approved profile, installs its
hooks, and exposes profile-backed ECS and world operations. Each host decides
when to initialize, tick, query, and shut it down.

## Development package

The Development package is for runtime maintainers. Its console, native
inspectors, captures, schemas, and function catalogs help gather and review
evidence for a specific executable build. These tools are never run by the
provider or by an end-user modloader. Drafts and raw captures stay in local
development data until reviewed and approved.

## What belongs in ShroudForge

ShroudForge owns its bootstrap adapter, settings, logging, user interface, and
time-limited loader health sessions. It reads the provider's status and
diagnostic snapshot through the public KFC Runtime API. It does not own a copy
of runtime implementation or profile-authoring tools.

## Data flow

```text
Executable build
       ↓
KFC Runtime developer tools gather evidence
       ↓
Draft profile and catalogs are reviewed and approved
       ↓
Approved profile is embedded in a Runtime build
       ↓
Any compatible native modloader loads the Runtime package
```

## Diagnostics boundary

The provider exposes cheap current-state information and bounded diagnostic
counters needed by any host. Build inspection, live ECS discovery, registry
audits, and profile evidence belong in `dev/tools/`. Loader-session controls,
mod callback measurements, logs, and UI status belong to the modloader that
owns them.
