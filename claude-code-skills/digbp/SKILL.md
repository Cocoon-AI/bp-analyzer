---
name: digbp
description: Analyze and edit Unreal Engine Blueprints via the digbp persistent server. Use when the user asks to inspect, export, search, understand, or MUTATE Blueprints — including editing variables, functions, components, nodes, and pin connections, reparenting, implementing interfaces, compiling, and saving. Also handles finding function callers, native events, asset references, and generating C++ migration stubs.
allowed-tools: Bash
argument-hint: "[command] [--flags]"
---

# digbp — Blueprint Analyzer CLI

`digbp` communicates with a persistent UE4 Blueprint Analyzer server over named pipes. The server stays running between commands, eliminating the ~60s UE4 startup cost on every query.

It supports **both read operations** (export, list, search, references) and **edit operations** (mutate variables, functions, components, nodes, save, compile). All read responses are JSON. All edit responses are JSON of the shape `{"success": bool, "error": "...", "path": "...", ...op-specific}`.

Configuration lives in `~/.digbp.yaml`. The binary is at `C:\tools\digbp.exe`.

## Server Lifecycle

The server auto-starts on first command. You can also manage it explicitly:

```bash
digbp start        # Launch UE4 server
digbp status       # Check if running
digbp stop         # Shutdown server
```

The first command in a session will be slow (UE4 startup). All subsequent commands are fast.

## Commands

### Export a Blueprint

```bash
# JSON output (default)
digbp export --path=/Game/Path/To/Blueprint

# Compact pseudocode
digbp export --path=/Game/Path/To/Blueprint --mode=compact

# C++ migration skeleton
digbp export --path=/Game/Path/To/Blueprint --mode=skeleton

# With complexity analysis
digbp export --path=/Game/Path/To/Blueprint --analyze

# Pretty-printed
digbp export --path=/Game/Path/To/Blueprint --pretty
```

### List Blueprints

```bash
digbp list --dir=/Game/UI/
digbp list --dir=/Game/UI/ --no-recurse
```

### C++ Function Usage

```bash
digbp cppusage --path=/Game/Path/To/Blueprint
```

### Asset References

```bash
digbp references --path=/Game/Path/To/Blueprint
```

### Dependency Graph

```bash
digbp graph --path=/Game/Path/To/Blueprint --depth=3
```

### Reference Viewer (Bidirectional)

```bash
# Default depths (3)
digbp refview --path=/Game/Path/To/Blueprint

# Custom depths
digbp refview --path=/Game/Path/To/Blueprint --refdepth=2 --referdepth=2

# Blueprints only (filter out textures, materials, etc.)
digbp refview --path=/Game/Path/To/Blueprint --bponly
```

### Find Function Callers

```bash
digbp findcallers --dir=/Game/ --func=GetPlayerController
digbp findcallers --dir=/Game/ --func=GetPlayerController --class=UGameplayStatics
```

### Find Event Implementations

```bash
# Native events (BlueprintNativeEvent)
digbp nativeevents --dir=/Game/

# Implementable events (BlueprintImplementableEvent)
digbp findevents --dir=/Game/ --event=ReceiveBeginPlay
```

### Find by CDO Property

```bash
digbp findprop --dir=/Game/ --prop=bCanBeDamaged --value=true
digbp findprop --dir=/Game/ --prop=bCanBeDamaged --parentclass=APawn
```

## Editing Blueprints (`digbp edit ...`)

All mutation commands live under `digbp edit`. They share a core discipline:

- **Edits stage in memory.** Nothing is written to disk until you explicitly call `digbp edit save`. Nothing recompiles until you call `digbp edit compile`.
- **No undo, no dry-run.** Source control (p4/git on the `.uasset`) is the rollback story. Before large changes, confirm the user is OK with it and ensure the file is checked in.
- **Batch where possible.** Make all related edits first, then `digbp edit save-and-compile` once at the end. Avoid compile-per-edit loops.
- **Success shape.** Every response is `{"success": true|false, "path": "...", ...}`. Check `.success` before assuming an edit landed. Use `--pretty` for readable output when debugging.

### Lifecycle

```bash
digbp edit compile          --path=/Game/Path/BP_Foo
digbp edit save             --path=/Game/Path/BP_Foo
digbp edit save-and-compile --path=/Game/Path/BP_Foo   # common ending
```

### Blueprint Metadata

```bash
# Change parent class (pass full /Script/... or /Game/... path)
digbp edit reparent --path=/Game/BP_Foo --parent-class=/Script/Engine.Actor

# Interfaces
digbp edit add-interface    --path=/Game/BP_Foo --interface=/Script/MyGame.MyInterface
digbp edit remove-interface --path=/Game/BP_Foo --interface=/Script/MyGame.MyInterface

# Flags — only the ones you pass are applied; omit to leave unchanged
digbp edit set-flags --path=/Game/BP_Foo --is-abstract --description="Base class for pickups"
```

### Variables

```bash
# Add a variable. Type syntax mirrors the reader's output:
#   primitives: bool, byte, int, int64, float, string, name, text
#   structs: vector, rotator, transform, color, linearcolor (aliases) or struct<Name>
#   object refs: object<ClassName> or object</Script/Engine.Actor>
#   class refs: class<ClassName>
#   containers: TArray<T>, TSet<T>, TMap<K,V>
digbp edit variable add --path=/Game/BP_Foo --name=Health --type=float \
    --default-value=100 --public --replicated --category="Stats"

digbp edit variable remove --path=/Game/BP_Foo --name=Health
digbp edit variable rename --path=/Game/BP_Foo --old-name=HP --new-name=Health
digbp edit variable set-type      --path=/Game/BP_Foo --name=Target --type=object<Actor>
digbp edit variable set-default   --path=/Game/BP_Foo --name=Health --value=75
digbp edit variable set-flags     --path=/Game/BP_Foo --name=Health --replicated --replication-condition=OwnerOnly
digbp edit variable set-metadata  --path=/Game/BP_Foo --name=Health --key=tooltip --value="Current health"
```

### CDO Properties (parent-class defaults)

For properties on the parent C++ class that don't live in `NewVariables` (e.g. `AActor::bCanBeDamaged`), use the CDO ops. Read first to learn the expected text format, then write:

```bash
digbp edit cdo get --path=/Game/BP_Foo --property=bCanBeDamaged
digbp edit cdo set --path=/Game/BP_Foo --property=bCanBeDamaged --value=true

# Struct values use UE's native text form:
digbp edit cdo set --path=/Game/BP_Foo --property=RelativeLocation --value="(X=0,Y=0,Z=100)"
```

### Functions

```bash
# Create a new user function
digbp edit function add --path=/Game/BP_Foo --name=GetHealthPct --pure --category="Stats"

# Parameters (add-param direction must be 'input' or 'output')
digbp edit function add-param    --path=/Game/BP_Foo --function=GetHealthPct --name=MaxHp --type=float --direction=input
digbp edit function add-param    --path=/Game/BP_Foo --function=GetHealthPct --name=Pct   --type=float --direction=output
digbp edit function remove-param --path=/Game/BP_Foo --function=GetHealthPct --name=MaxHp

digbp edit function rename    --path=/Game/BP_Foo --old-name=Foo --new-name=Bar
digbp edit function set-flags --path=/Game/BP_Foo --function=Bar --pure --const
digbp edit function remove    --path=/Game/BP_Foo --name=Bar

# Create an override graph for a parent-class BlueprintImplementable/NativeEvent
digbp edit function override --path=/Game/BP_Foo --function=ReceiveBeginPlay
```

### Events

```bash
# Custom event with typed parameters. --param is repeatable as Name:Type.
digbp edit event add-custom --path=/Game/BP_Foo --name=OnDeath \
    --param="Killer:object<Controller>" --param="Damage:float"

digbp edit event remove    --path=/Game/BP_Foo --name=OnDeath

# Stub implementation of a parent BlueprintEvent
digbp edit event implement --path=/Game/BP_Foo --event=ReceiveBeginPlay
```

### Components (Simple Construction Script)

```bash
# Pass class by full path; --parent is optional (omit to attach at root)
digbp edit component add --path=/Game/BP_Foo --name=Mesh \
    --class=/Script/Engine.StaticMeshComponent --parent=DefaultSceneRoot

digbp edit component remove   --path=/Game/BP_Foo --name=Mesh
digbp edit component reparent --path=/Game/BP_Foo --name=Mesh --new-parent=Body

# Set a property on the component template (archetype). Value uses UE text format.
digbp edit component set-property --path=/Game/BP_Foo --component=Mesh \
    --property=RelativeLocation --value="(X=0,Y=0,Z=50)"
```

### Node Graph Editing

All node ops take `--path` + `--graph`. Graph name is the function name, or `EventGraph` for the main ubergraph. Nodes are addressed by **GUID** (returned when you create them). Pins are addressed by `{node_guid, pin_name}` — pins have no stable GUID in UE4.

**Important workflow:** `add-*` responses include `"node": {"guid": "...", "pins": [...]}`. Parse the `guid` to wire subsequent connections. Don't re-export the entire BP just to get GUIDs.

```bash
# High-level curated builders (cover ~90% of use cases)
digbp edit node add-variable-get  --path=/Game/BP_Foo --graph=EventGraph --variable=Health --x=0 --y=0
digbp edit node add-variable-set  --path=/Game/BP_Foo --graph=EventGraph --variable=Health --x=400 --y=0
digbp edit node add-function-call --path=/Game/BP_Foo --graph=EventGraph --function=Less_FloatFloat --class=UKismetMathLibrary --x=200 --y=0
digbp edit node add-branch        --path=/Game/BP_Foo --graph=EventGraph --x=400 --y=0
digbp edit node add-sequence      --path=/Game/BP_Foo --graph=EventGraph --then-pin-count=3
digbp edit node add-cast          --path=/Game/BP_Foo --graph=EventGraph --target-class=/Script/Engine.Pawn --pure
digbp edit node add-make-struct   --path=/Game/BP_Foo --graph=EventGraph --struct=Vector
digbp edit node add-break-struct  --path=/Game/BP_Foo --graph=EventGraph --struct=Transform
digbp edit node add-literal       --path=/Game/BP_Foo --graph=EventGraph --object=/Game/Textures/T_Icon.T_Icon

# Escape hatch for anything not in the curated set
digbp edit node add-generic --path=/Game/BP_Foo --graph=EventGraph \
    --node-class=/Script/BlueprintGraph.K2Node_Self

# Node edits (need the GUID from a prior add response or from `digbp export`)
digbp edit node move   --path=/Game/BP_Foo --graph=EventGraph --node-guid=<GUID> --x=500 --y=100
digbp edit node remove --path=/Game/BP_Foo --graph=EventGraph --node-guid=<GUID>
```

### Pin Wiring

```bash
# Connect output pin -> input pin
digbp edit pin connect --path=/Game/BP_Foo --graph=EventGraph \
    --from-node=<GUID_A> --from-pin=ReturnValue \
    --to-node=<GUID_B>   --to-pin=Condition

# Disconnect a specific link
digbp edit pin disconnect --path=/Game/BP_Foo --graph=EventGraph \
    --from-node=<GUID_A> --from-pin=Then \
    --to-node=<GUID_B>   --to-pin=Execute

# Break all connections on a single pin
digbp edit pin break-all-links --path=/Game/BP_Foo --graph=EventGraph \
    --node-guid=<GUID> --pin-name=Exec

# Set a literal default on an input pin (prefer this over add-literal for scalars)
digbp edit pin set-default --path=/Game/BP_Foo --graph=EventGraph \
    --node-guid=<GUID> --pin-name=B --value=0.5
```

### Worked Example: build a small graph from scratch

```bash
# 1. Add a Health variable
digbp edit variable add --path=/Game/BP_Foo --name=Health --type=float --default-value=100 --public

# 2. Create a pure helper function
digbp edit function add --path=/Game/BP_Foo --name=IsAlive --pure
digbp edit function add-param --path=/Game/BP_Foo --function=IsAlive --name=ReturnValue --type=bool --direction=output

# 3. Wire the graph — capture each node's GUID from the response JSON
GET_JSON=$(digbp edit node add-variable-get --path=/Game/BP_Foo --graph=IsAlive --variable=Health --x=0 --y=0 --pretty)
GET_GUID=$(echo "$GET_JSON" | jq -r .node.guid)

GT_JSON=$(digbp edit node add-function-call --path=/Game/BP_Foo --graph=IsAlive --function=Greater_FloatFloat --class=UKismetMathLibrary --x=200 --y=0 --pretty)
GT_GUID=$(echo "$GT_JSON" | jq -r .node.guid)

# 4. Connect Health -> A on the comparison, set B default to 0
digbp edit pin connect --path=/Game/BP_Foo --graph=IsAlive \
    --from-node=$GET_GUID --from-pin=Health \
    --to-node=$GT_GUID    --to-pin=A
digbp edit pin set-default --path=/Game/BP_Foo --graph=IsAlive \
    --node-guid=$GT_GUID --pin-name=B --value=0

# 5. Commit
digbp edit save-and-compile --path=/Game/BP_Foo
```

### Common Failure Modes

- **"Missing required param: X"** — you omitted a `--flag`. Every `required` flag is documented in `digbp edit <command> --help`.
- **"Could not resolve parent class: X"** — use the full path (`/Script/Engine.Actor`) or the short name with or without the `A`/`U` prefix. The plugin tries several fallbacks but isn't magic.
- **"TryCreateConnection failed: <schema message>"** — the schema rejected the connection. The error includes the schema's diagnostic (type mismatch, direction mismatch, etc.). Read it, fix the pin types, retry.
- **"Blueprint has no GeneratedClass; compile it first"** — `edit cdo get/set` needs a compiled class. Run `digbp edit compile --path=...` first.
- **Compile errors after structural edits** — run `digbp edit compile --path=...` and inspect the `compile.status` field. `error` means the BP is broken; check the editor for the actual message.

## Known Gotchas

### Git Bash Path Conversion
Git Bash on Windows converts `/Game/...` paths to local filesystem paths (e.g. `C:/Users/.../Git/Game/...`), which makes digbp return empty results. The fix is to set `MSYS_NO_PATHCONV=1` as a **Windows user environment variable** (Win+R → `sysdm.cpl` → Advanced → Environment Variables → User variables → New). After setting it, fully restart Claude Code so the harness inherits it.

If this is unset for any reason, every command must be prefixed inline:
```bash
MSYS_NO_PATHCONV=1 digbp export --path=/Game/Showdown/...
```
Quick check: `echo $MSYS_NO_PATHCONV` should print `1`. If empty, the env var did not propagate — fall back to the inline prefix.

**Note:** `MSYS2_ARG_CONV_EXCL=/Game/;/Script/;/Engine/` does NOT work for digbp's `--path=/Game/...` style arguments. The exclusion list matches the start of the entire argument (`--path=`), not the embedded path. Use `MSYS_NO_PATHCONV=1` instead.

### Calling BP-Defined Functions/Events with add-function-call
When calling a function or custom event defined in the **blueprint itself** (not on a C++ parent class):
- `--class=MyBlueprint_C` will **fail** — the generated class may not expose the function to the lookup.
- `--class=SKEL_MyBlueprint_C` **works** — use the skeleton class prefix.

Example:
```bash
# FAILS:
digbp edit node add-function-call --function=MyBPEvent --class=BP_Foo_C
# WORKS:
digbp edit node add-function-call --function=MyBPEvent --class=SKEL_BP_Foo_C
```

### Newly Added Custom Events Are Not Immediately Callable
After `digbp edit event add-custom`, the event exists in the graph but is not yet registered as a callable function on the class. You must **compile first** before other nodes can call it:
```bash
digbp edit event add-custom --path=/Game/BP_Foo --name=MyEvent --param="Tag:name"
digbp edit compile --path=/Game/BP_Foo          # <-- required before add-function-call
digbp edit node add-function-call --function=MyEvent --class=SKEL_BP_Foo_C ...
```

### p4 Edit Before Mutating
The `.uasset` must be writable. Always `p4 edit` the content file before any `digbp edit` commands. The content path follows the pattern:
```
Showdown/Content/Showdown/UI/...  (not Showdown/Content/UI/...)
```
Use `find Showdown/Content -iname "BlueprintName*"` if unsure of the exact path.

### Server Persistence Across Sessions
The server does **not** survive between Claude Code sessions. If you get `EOF` or `server not running` errors, the server died. Any command will auto-restart it (~60s), or use `digbp start` explicitly.

## Output

All commands return JSON to stdout. Use `--pretty` for indented output. Errors go to stderr.

When the user asks to analyze a blueprint by name without a full path, first use `digbp list` to find it:

```bash
MSYS_NO_PATHCONV=1 digbp list --dir=/Game/ | jq '.blueprints[] | select(.name | test("PartialName"; "i"))'
```

## Resolving Partial Names

If the user gives a short name like "GameInstance" instead of a full path:
1. Use `digbp list --dir=/Game/ --pretty` and search the output for a matching name
2. Then use the full path from the result in subsequent commands

## Timeout

Use a Bash timeout of at least 120000ms for the first command in a session (server startup). Subsequent commands complete in seconds.
