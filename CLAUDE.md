# CLAUDE.md

This file provides guidance to Claude Code when working with the Blueprint Analyzer project.

## Project Overview

UE4.27 Editor plugin providing a CLI commandlet for read-only Blueprint analysis. Enables AI tools to inspect Blueprint structure, logic flow, and C++ function usage for debugging and migration assistance.

## Related Projects

- **Grit**: Western-themed battle royale game using UE4.27
  - Game directory: `D:\sd\dev`
  - Main game code: `D:\sd\dev\Showdown\`
  - Plugin installed at: `D:\sd\dev\Engine\Plugins\Marketplace\BlueprintAnalyzer\`
  - Use `mcp-perforce` for version control

## Architecture

Direct CLI invocation without requiring running editor UI:

```
AI Tool -> Bash -> UE4Editor-Cmd.exe -run=BlueprintExport -> stdout
```

Output wrapped in `__JSON_START__...__JSON_END__` markers for reliable parsing.

## Commandlet Usage

```bash
# Export Blueprint (compact pseudocode format - default)
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -path=/Game/Showdown/UI/MainMenu

# Export as full JSON
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -json

# Generate C++ migration skeleton
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -skeleton

# Get C++ function usage
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -cppusage

# Get asset references
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -references

# Export dependency graph
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -graph -depth=3

# List Blueprints in directory
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/Showdown/UI/

# Find Blueprints calling a function
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/ -func=GetPlayerController -class=UGameplayStatics

# Find native event implementations
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/ -nativeevents

# Find implementable event implementations
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/ -event=ReceiveBeginPlay

# Include analysis metrics with JSON
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -json -analyze

# Write output to file
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -json -out=output.json
```

**Git Bash Note**: Use `MSYS_NO_PATHCONV=1` prefix to prevent path mangling.

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
- Optional analysis metrics with `-analyze`

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
| Find Callers | `-dir=... -func=Name` | Find function callers |
| Find Callers (filtered) | `-dir=... -func=Name -class=Class` | Find with class filter |
| Native Events | `-dir=... -nativeevents` | Find native event implementations |
| Implementable Events | `-dir=... -event=Name` | Find implementable event implementations |
| Analyze | `-json -analyze` | Include complexity metrics |
| File Output | `-out=file.json` | Write to file instead of stdout |

## Project Structure

```
bp-analyzer/
├── BlueprintAnalyzer/                    # UE4 Plugin
│   ├── BlueprintAnalyzer.uplugin
│   └── Source/BlueprintAnalyzer/
│       ├── BlueprintAnalyzer.Build.cs
│       ├── Public/
│       │   ├── BlueprintExportData.h     # Data structures (10 USTRUCTs)
│       │   └── BlueprintExportReader.h   # Reader UCLASS API
│       └── Private/
│           ├── BlueprintExportReader.cpp # Core implementation (~1300 lines)
│           ├── BlueprintExportCommandlet.cpp # CLI + output formatting (~1300 lines)
│           ├── BlueprintExportCommandlet.h
│           └── BlueprintAnalyzerModule.*
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
| `FBlueprintVariableData` | Variable: name, type, category, default, public/readonly/visible flags, replication settings |
| `FBlueprintFunctionData` | Function: name, category, inputs, outputs, pure/static/const/override flags, nodes, connections |
| `FBlueprintEventData` | Event: name, type (Custom/NativeEvent/ImplementableEvent/Event), nodes, connections |
| `FBlueprintDispatcherData` | Event dispatcher: name, parameters, category, tooltip |
| `FBlueprintReferenceData` | Asset reference: path, type, hard/soft flag, context |
| `FBlueprintComponentData` | Component: name, class, parent, root flag, transform |
| `FBlueprintCppFunctionUsage` | C++ call: function name, class, blueprint path, node GUID, graph name, callable/native/implementable flags |

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

## Grit Blueprint Locations

- `/Game/Showdown/UI/` - User interface widgets
- `/Game/Showdown/Blueprints/` - Game logic
- `/Game/Showdown/Characters/` - Character blueprints
- `/Game/Showdown/Modes/` - Game mode blueprints
- `/Game/Showdown/Weapons/` - Weapon blueprints

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

### Testing

```bash
MSYS_NO_PATHCONV=1 "D:/sd/dev/Engine/Binaries/Win64/UE4Editor-Cmd.exe" \
  "D:/sd/dev/Showdown/Showdown.uproject" \
  -run=BlueprintExport \
  -path=/Game/Showdown/Modes/SDGameInstance_BP
```
