# CLAUDE.md

This file provides guidance to Claude Code when working with the Blueprint Analyzer project.

## Project Overview

UE4.27 Editor plugin providing a CLI commandlet for Blueprint analysis and editing. Enables AI tools to inspect Blueprint structure, logic flow, and C++ function usage for debugging and migration assistance, and to mutate Blueprints (variables, functions, events, dispatchers, nodes, components) via JSON-RPC.

## Architecture

Two modes of operation:

### CLI Mode (one-shot)
```
AI Tool -> Bash -> UE4Editor-Cmd.exe -run=BlueprintExport -> stdout
```
Output wrapped in `__JSON_START__...__JSON_END__` markers for reliable parsing.

### Server Mode (persistent, recommended)
```
AI Tool -> digbp CLI -> Named Pipe -> UE4 Server (stays running) -> JSON-RPC response
```
The server keeps UE4 loaded, eliminating the ~30-60s startup per query. The `digbp` Go CLI manages the server lifecycle and communicates via Windows Named Pipes using JSON-RPC 2.0 with 4-byte length-prefix framing.

## Commandlet Usage

```bash
# Export Blueprint (compact pseudocode format - default)
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/Blueprints/MyBP

# Export as full JSON
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/Blueprints/MyBP -json

# Generate C++ migration skeleton
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -skeleton

# Get C++ function usage
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -cppusage

# Get asset references
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -references

# Export dependency graph
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -graph -depth=3

# List Blueprints in directory
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/UI/

# Find Blueprints calling a function
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/ -func=GetPlayerController -class=UGameplayStatics

# Find Blueprints reading/writing a variable (K2Node_VariableGet/Set sites)
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/ -var=ShopPopup
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/ -var=ShopPopup -varkind=set

# Find native event implementations
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/ -nativeevents

# Find implementable event implementations
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/ -event=ReceiveBeginPlay

# Find Blueprints by CDO property value
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/ -findprop=bCanBeDamaged -propvalue=true

# Reference viewer (bidirectional dependency graph)
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -refview -refdepth=2 -referdepth=2

# Include analysis metrics with JSON
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -json -analyze

# Write output to file
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -json -out=output.json
```

**Git Bash Note**: Use `MSYS_NO_PATHCONV=1` prefix to prevent path mangling.

## digbp CLI Tool (Recommended)

The `digbp` Go CLI provides a persistent server mode that avoids repeated UE4 startup costs.

### Setup

Create `~/.digbp.yaml`:
```yaml
editor_cmd: "E:/path/to/UE4Editor-Cmd.exe"
uproject: "E:/path/to/Project.uproject"
pipe_name: "blueprintexport"    # optional, this is the default
start_timeout: 120s             # optional, default 120s
```

Or use environment variables: `DIGBP_EDITOR_CMD`, `DIGBP_UPROJECT`, `DIGBP_PIPE_NAME`.

Build: `cd bp-analyzer && go build -o digbp.exe ./cmd/digbp/`

### Server Lifecycle

```bash
digbp start                     # Launch UE4 server (auto-starts on first command too)
digbp status                    # Check if server is running
digbp stop                      # Shutdown server
digbp version                   # Report CLI and server build timestamps (detects PATH staleness)
```

### digbp Commands

```bash
# Export Blueprint (JSON by default)
digbp export --path=/Game/Blueprints/MyBP
digbp export --path=/Game/Blueprints/MyBP --mode=compact
digbp export --path=/Game/Blueprints/MyBP --mode=skeleton
digbp export --path=/Game/Blueprints/MyBP --analyze

# List Blueprints in directory
digbp list --dir=/Game/UI/
digbp list --dir=/Game/UI/ --no-recurse

# C++ function usage
digbp cppusage --path=/Game/Blueprints/MyBP

# Asset references
digbp references --path=/Game/Blueprints/MyBP

# Dependency graph
digbp graph --path=/Game/Blueprints/MyBP --depth=3

# Reference viewer (bidirectional)
digbp refview --path=/Game/Blueprints/MyBP --refdepth=2 --referdepth=2
digbp refview --path=/Game/Blueprints/MyBP --bponly

# Find function callers
digbp findcallers --dir=/Game/ --func=GetPlayerController --class=UGameplayStatics

# Find variable get/set sites
digbp findvaruses --dir=/Game/ --var=ShopPopup
digbp findvaruses --dir=/Game/ --var=ShopPopup --kind=set

# Find native event implementations
digbp nativeevents --dir=/Game/

# Find implementable event implementations
digbp findevents --dir=/Game/ --event=ReceiveBeginPlay

# Find by CDO property
digbp findprop --dir=/Game/ --prop=bCanBeDamaged --value=true
digbp findprop --dir=/Game/ --prop=bCanBeDamaged --parentclass=APawn

# Text search across Blueprints (like Find in Blueprints)
digbp search --dir=/Game/UI/ --query=PlayAnimation
digbp search --dir=/Game/ --query=NFTRecipe --pretty

# C++ reference audit (reverse-index: which BPs touch which native symbol)
# Use before deleting C++ to find BPs that will break.
digbp cpp-audit --dir=/Game/
digbp cpp-audit --dir=/Game/ --out=cpp-refs.json
digbp cpp-audit --dir=/Game/Showdown/ --pretty

# Emit C++ UPROPERTY declarations from BP metadata (for BP→C++ variable lift)
digbp cppgen upropertys --path=/Game/BP
digbp cppgen upropertys --path=/Game/BP --vars="CurrentXP,OnReady" --category="AccountInfo"

# Pretty-print JSON output
digbp export --path=/Game/BP --pretty
```

### Edit Commands (Blueprint Mutation)

All edit operations stage changes in memory until explicitly saved with `edit save` or `edit save-and-compile`.

```bash
# Compile / save
digbp edit compile --path=/Game/BP
digbp edit save --path=/Game/BP
digbp edit save-and-compile --path=/Game/BP

# Variables
digbp edit variable list --path=/Game/BP                          # List member variables
digbp edit variable list --path=/Game/BP --include-broken         # Include vars with deleted types
digbp edit variable add --path=/Game/BP --name=Health --type=float
digbp edit variable remove --path=/Game/BP --name=Health
digbp edit variable remove --path=/Game/BP --name=Broken --force  # Force-remove broken-type vars
digbp edit variable rename --path=/Game/BP --old-name=Hp --new-name=Health
digbp edit variable unshadow --path=/Game/BP --dry-run            # Retarget <X>_0 shadow refs → <X>
digbp edit variable unshadow --path=/Game/BP                      # Apply
digbp edit variable lift --path=/Game/BP --vars="Current XP,XP Level Threshold"  # Rename-to-CPP-friendly + remove + retarget external BPs (atomic)
digbp edit variable lift --path=/Game/BP --vars="..." --dry-run
digbp edit variable lift --path=/Game/BP --vars="..." --scope=/Game/UI/         # Narrow external-BP scan
digbp edit variable lift --path=/Game/BP --vars="..." --no-scan-external        # Skip external scan (verify with findvaruses first)
digbp edit variable set-type --path=/Game/BP --name=Health --type=int
digbp edit variable set-default --path=/Game/BP --name=Health --value=100
digbp edit variable set-flags --path=/Game/BP --name=Health --public --replicated

# Functions
digbp edit function add --path=/Game/BP --name=DoStuff
digbp edit function remove --path=/Game/BP --name=DoStuff
digbp edit function remove --path=/Game/BP --name="Play SFX" --retarget-external-to=PlaySFX  # Retarget external K2Node_CallFunction refs
digbp edit function rename --path=/Game/BP --old-name=Foo --new-name=Bar

# Events
digbp edit event add-custom --path=/Game/BP --name=OnReady
digbp edit event remove --path=/Game/BP --name=OnReady
digbp edit event implement --path=/Game/BP --event=ReceiveBeginPlay

# Event dispatchers
digbp edit dispatcher remove --path=/Game/BP --name=OnUpdate

# Components
digbp edit component add --path=/Game/BP --name=Mesh --class=StaticMeshComponent
digbp edit component remove --path=/Game/BP --name=Mesh

# Nodes
digbp edit node remove --path=/Game/BP --graph=EventGraph --node-guid=GUID
digbp edit node remove-broken --path=/Game/BP --dry-run           # Preview broken nodes
digbp edit node remove-broken --path=/Game/BP                     # Remove all broken nodes
digbp edit node add-function-call --path=/Game/BP --graph=EventGraph --function=PrintString

# Cleanup tools
digbp edit purge-phantom --path=/Game/BP --property=DeletedVar --dry-run  # Find phantom property
digbp edit purge-phantom --path=/Game/BP --property=DeletedVar            # Remove + recompile

# Blueprint metadata
digbp edit reparent --path=/Game/BP --parent-class=/Script/Engine.Actor
digbp edit add-interface --path=/Game/BP --interface=MyInterface
digbp edit remove-interface --path=/Game/BP --interface=MyInterface
digbp edit set-flags --path=/Game/BP --is-abstract=true
```

### Auto-Start

All operation commands automatically start the server if it's not running. The first call will have the UE4 startup delay; subsequent calls are fast.

## Output Modes

| Mode | Flag | Description |
|------|------|-------------|
| Compact | *(default)* | Pseudocode format - concise, readable logic flow |
| JSON | `-json` | Full JSON with all nodes, pins, connections |
| Skeleton | `-skeleton` | C++ migration stubs with BP logic as comments |

### Compact Output

Readable pseudocode showing logic flow:
- Variables with type, visibility, replication
- Functions with parameters and logic
- Events with execution flow
- Filters out reroute/knot nodes
- Marks C++ calls with `// C++` comments

### JSON Output

Complete graph representation:
- All nodes with GUID, type, title, position
- All pins with types, defaults, connections
- Full connection maps
- Component hierarchy
- Locally-defined macros under top-level `macros` (same shape as functions: inputs/outputs/nodes/connections; inputs come from tunnel entry node output pins, outputs from tunnel exit node input pins)
- Optional analysis metrics with `-analyze` (also available in compact/skeleton modes as a comment header)

### Skeleton Output

C++ migration stubs:
- UCLASS/USTRUCT with GENERATED_BODY()
- UPROPERTY with appropriate specifiers
- UFUNCTION stubs with BP logic as comments
- Type conversion (bool, int32, FString, FVector, etc.)

## Commandlet Operations

| Operation | Flags | Description |
|-----------|-------|-------------|
| Export | `-path=/Game/...` | Export single Blueprint |
| List | `-dir=/Game/...` | List Blueprints in directory |
| List (non-recursive) | `-dir=/Game/... -norecurse` | List without subdirectories |
| C++ Usage | `-path=... -cppusage` | Get C++ function calls |
| References | `-path=... -references` | Get asset dependencies |
| Graph | `-path=... -graph -depth=N` | Export dependency graph |
| Reference Viewer | `-path=... -refview` | Bidirectional reference graph |
| Reference Viewer (depths) | `-path=... -refview -refdepth=N -referdepth=N` | Custom depths |
| Reference Viewer (BP only) | `-path=... -refview -bponly` | Only include Blueprints |
| Find Callers | `-dir=... -func=Name` | Find function callers |
| Find Callers (filtered) | `-dir=... -func=Name -class=Class` | Find with class filter |
| Find Var Uses | `-dir=... -var=Name` | Find K2Node_VariableGet/Set sites |
| Find Var Uses (filtered) | `-dir=... -var=Name -varkind=get\|set` | Restrict to get or set only |
| Native Events | `-dir=... -nativeevents` | Find native event implementations |
| Implementable Events | `-dir=... -event=Name` | Find implementable event implementations |
| Property Search | `-dir=... -findprop=Name` | Find Blueprints with CDO property |
| Property Search (filtered) | `-dir=... -findprop=Name -propvalue=Value` | Filter by property value |
| Property Search (by class) | `-dir=... -findprop=Name -parentclass=Class` | Filter by parent class |
| Text Search | `-dir=... -search=Text` | Find in Blueprints (nodes, pins, comments, variables) |
| Cpp Audit | `-dir=... -cppaudit` | Reverse-index of every native C++ symbol any BP references |
| Analyze | `-json -analyze` | Include complexity metrics |
| File Output | `-out=file.json` | Write to file instead of stdout |
| Server Mode | `-pipeserver` | Start persistent named pipe server |
| Server Mode (custom) | `-pipeserver -pipename=Name` | Custom pipe name |

## Project Structure

```
bp-analyzer/
├── BlueprintAnalyzer/                    # UE4 Plugin
│   ├── BlueprintAnalyzer.uplugin
│   └── Source/BlueprintAnalyzer/
│       ├── BlueprintAnalyzer.Build.cs
│       ├── Public/
│       │   ├── BlueprintExportData.h     # Data structures (14 USTRUCTs)
│       │   └── BlueprintExportReader.h   # Reader UCLASS API
│       └── Private/
│           ├── BlueprintExportReader.cpp # Core read/search implementation
│           ├── BlueprintExportCommandlet.cpp # CLI + output formatting
│           ├── BlueprintExportCommandlet.h
│           ├── BlueprintExportServer.cpp # Named pipe server + JSON-RPC dispatch
│           ├── BlueprintExportServer.h
│           ├── BlueprintExportServerEditDispatch.cpp # Edit method routing
│           ├── BlueprintEditOps.h        # Edit operation declarations
│           ├── BlueprintEditHelpers.*    # Shared edit utilities
│           ├── BlueprintEditOps_Metadata.cpp # Compile, save, reparent, interfaces
│           ├── BlueprintEditOps_Variables.cpp # Variable CRUD, CDO, purge-phantom
│           ├── BlueprintEditOps_Functions.cpp # Function/event/dispatcher ops
│           ├── BlueprintEditOps_Components.cpp # Component add/remove/reparent
│           ├── BlueprintEditOps_Nodes.cpp    # Node remove/move/add, remove-broken
│           ├── BlueprintEditOps_NodeBuilders.cpp # High-level node builders
│           └── BlueprintAnalyzerModule.*
├── cmd/digbp/
│   ├── main.go                           # digbp CLI tool (Go)
│   ├── edit.go                           # Edit command group
│   ├── edit_variable.go                  # Variable subcommands
│   ├── edit_function.go                  # Function/event/dispatcher subcommands
│   ├── edit_node.go                      # Node/pin subcommands
│   └── edit_component.go                 # Component subcommands
├── internal/
│   ├── config/config.go                  # Config file loading (~/.digbp.yaml)
│   ├── pipe/client.go                    # Named pipe client with length framing
│   ├── rpc/rpc.go                        # JSON-RPC 2.0 client
│   └── server/manager.go                 # UE4 server lifecycle management
├── go.mod
├── claude-code-skills/
│   └── blueprint-export/SKILL.md         # Claude Code skill definition
├── CLAUDE.md                             # This file
└── README.md
```

## Data Structures

| Structure | Purpose |
|-----------|---------|
| `FBlueprintExportData` | Complete Blueprint: name, path, parent, type, interfaces, variables, functions, events, dispatchers, references, components, metadata flags |
| `FBlueprintNodeData` | Node: GUID, type, title, comment, input/output pins, position, function name/class, native function flag |
| `FBlueprintPinData` | Pin: name, type, default value, array/reference/const flags, linked connections |
| `FBlueprintConnectionData` | Graph edge: source node GUID + pin, target node GUID + pin |
| `FBlueprintVariableData` | Variable: name, type, category, default, public/readonly/visible flags, replication settings, `bIsTypeBroken` flag |
| `FBlueprintFunctionData` | Function: name, category, inputs, outputs, pure/static/const/override flags, nodes, connections |
| `FBlueprintEventData` | Event: name, type (Custom/NativeEvent/ImplementableEvent/Event), nodes, connections |
| `FBlueprintDispatcherData` | Event dispatcher: name, parameters, category, tooltip |
| `FBlueprintReferenceData` | Asset reference: path, type, hard/soft flag, context |
| `FBlueprintComponentData` | Component: name, class, parent, root flag, transform |
| `FBlueprintCppFunctionUsage` | C++ call: function name, class, blueprint path, node GUID, graph name, callable/native/implementable flags |
| `FBlueprintPropertySearchResult` | Property search result: blueprint path/name, parent class, property name/value/type |
| `FAssetReferenceNode` | Reference graph node: path, name, class, depth, dependencies, referencers, hard/soft/blueprint/native flags |
| `FAssetReferenceGraph` | Reference viewer graph: root path, depths, nodes map, dependency/referencer/blueprint/native counts |
| `FBlueprintSearchResult` | Search hit: blueprint path, graph name, node GUID, match field (NodeTitle/NodeComment/PinName/PinDefault/VariableName/GraphName), match value, node class |

## C++ Function Detection

The plugin tracks C++ function usage:
- **BlueprintCallable**: Functions callable from Blueprint
- **BlueprintNativeEvent**: C++ functions with Blueprint override capability
- **BlueprintImplementableEvent**: Blueprint-only event implementations
- **Native functions**: FUNC_Native flag set

Detection uses UE4.27 pattern: `FUNC_BlueprintEvent + FUNC_Native` for BlueprintNativeEvent (no separate flag in 4.27).

## Type Conversion (BP -> C++)

| Blueprint Type | C++ Type |
|----------------|----------|
| `object<ClassName>` | `UClassName*` |
| `class<ClassName>` | `TSubclassOf<UClassName>` |
| `bool` | `bool` |
| `int`/`integer` | `int32` |
| `float`/`real` | `float` |
| `string` | `FString` |
| `name` | `FName` |
| `text` | `FText` |
| `vector` | `FVector` |
| `rotator` | `FRotator` |
| `transform` | `FTransform` |
| Arrays | `TArray<T>` |
| Sets | `TSet<T>` |
| Maps | `TMap<K, V>` |

## Development Guidelines

### Plugin Development

Source code in `BlueprintAnalyzer/Source/`:
- `Public/` - Data structures and reader API
- `Private/` - Implementation and commandlet

Module type is `Editor` (requires editor, loads at Default phase).

### UE4.27 Compatibility

- Use `FUNC_BlueprintEvent + FUNC_Native` for BlueprintNativeEvent detection
- Use `FindMetaData()` not `GetMetaData()` (returns pointer)
- Include `UObject/Script.h` for FUNC_* flags
- `Blueprint->PropertyGuids` does NOT exist in UE4.27 (UE5 only)
- `UEdGraph::LocalVariables` does NOT exist in UE4.27 (UE5 only)
- UE4 uses unity builds — static functions in anonymous namespaces across .cpp files can collide. Use unique prefixes (e.g., `EditOps_PinTypeToString`)
- All graph iteration must include `Blueprint->DelegateSignatureGraphs` alongside FunctionGraphs/UbergraphPages/MacroGraphs — dispatchers store parameters as pins on function entry nodes in these graphs

### Testing

```bash
MSYS_NO_PATHCONV=1 "Path/To/Engine/Binaries/Win64/UE4Editor-Cmd.exe" \
  "Path/To/Project.uproject" \
  -run=BlueprintExport \
  -path=/Game/Blueprints/MyBP
```
