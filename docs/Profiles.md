# Runtime profiles

Production profiles contain only data required by the provider for one game,
target, and executable build. They live under:

```text
profiles/<game>/<target>/<build>.json
profiles/<game>/<target>/<game>-<target>-<build>.components.json
```

For example, the current Enshrouded client profile is
`profiles/enshrouded/client/1076226.json`, with its component map alongside it.
The provider selects a profile by executable identity. CMake embeds approved
profiles and their component maps into the Runtime DLL.

Function signatures, scan observations, reflection imports, raw captures, and
unapproved values are development evidence. They belong in `dev/` or ignored
local `devdata/`, not in the production profile unless a reviewed runtime field
requires them.

A profile is promoted only after identity, provenance, component mappings, and
required live observations have been reviewed. The development console
validates the schema and approval evidence. CMake rejects profiles that are not
approved. See [the development workflow](../dev/README.md).
