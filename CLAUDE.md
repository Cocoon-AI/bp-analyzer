# CLAUDE.md

This file provides guidance to Claude Code when working with the Blueprint Analyzer project.

## Project Overview

UE4 plugin with CLI commandlet for analyzing Unreal Engine 4.27 Blueprints. Provides read-only Blueprint analysis for debugging and C++ migration assistance.

## Related Projects

- **Grit**: Western-themed battle royale game using UE4.27
  - Game directory: `D:\sd\dev`
  - Main game code: `D:\sd\dev\Showdown\`
  - Plugin installed at: `D:\sd\dev\Engine\Plugins\Marketplace\BlueprintAnalyzer\`
  - Use `mcp-perforce` for version control

## Architecture

Direct CLI invocation without requiring running editor:

```
Claude Code → Bash → UE4Editor-Cmd.exe -run=BlueprintExport → JSON output
```

## Commandlet Usage

```bash
# Export Blueprint (compact pseudocode format)
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -path=/Game/Showdown/UI/MainMenu

# Export as full JSON
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -path=/Game/BP -json

# Generate C++ migration skeleton
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -path=/Game/BP -skeleton

# C++ function usage
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -path=/Game/BP -cppusage

# Find Blueprints in directory
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -dir=/Game/Showdown/UI/

# Find Blueprints calling a function
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -dir=/Game/ -func=GetPlayerController

# Find native event implementations
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -dir=/Game/ -nativeevents

# With analysis metrics
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -path=/Game/BP -json -analyze
```

**Git Bash Note**: Use `MSYS_NO_PATHCONV=1` prefix to prevent path mangling.

## Output Modes

| Mode | Flag | Description |
|------|------|-------------|
| Compact | *(default)* | Pseudocode format - concise, readable logic flow |
| JSON | `-json` | Full JSON with all nodes, pins, and connections |
| Skeleton | `-skeleton` | C++ migration stubs with BP logic as comments |

## Output Format

JSON wrapped in markers for reliable parsing:
```
__JSON_START__{"success":true,"blueprint_name":"MyBlueprint",...}__JSON_END__
```

## Project Structure

```
bp-analyzer/
├── BlueprintAnalyzer/              # UE4 Plugin (ready to install)
│   ├── BlueprintAnalyzer.uplugin
│   └── Source/BlueprintAnalyzer/
│       ├── Public/
│       │   ├── BlueprintExportData.h   # All USTRUCT definitions
│       │   └── BlueprintExportReader.h # Reader UCLASS
│       └── Private/
│           ├── BlueprintExportReader.cpp    # Implementation
│           ├── BlueprintExportCommandlet.*  # CLI commandlet
│           └── BlueprintAnalyzerModule.*
├── claude-code-skills/             # Claude Code skill definitions
│   └── blueprint-export/SKILL.md
├── docs/
│   ├── plan.md                     # Project objectives
│   ├── status.md                   # Current status
│   └── commandlet_implementation_plan.md
└── README.md
```

## Plugin Data Structures

| Structure | Purpose |
|-----------|---------|
| `FBlueprintExportData` | Complete Blueprint: name, path, parent, variables, functions, events |
| `FBlueprintNodeData` | Node info with C++ function metadata |
| `FBlueprintConnectionData` | Graph connections between nodes |
| `FBlueprintVariableData` | Variables with type, defaults, replication |
| `FBlueprintFunctionData` | Functions with inputs, outputs, graph |
| `FBlueprintCppFunctionUsage` | C++ call tracking (BlueprintCallable/NativeEvent) |

## Commandlet Operations

| Operation | Flags | Description |
|-----------|-------|-------------|
| Export | `-path=/Game/...` | Export single Blueprint |
| List | `-dir=/Game/...` | List Blueprints in directory |
| Analyze | `-json -analyze` | Include complexity metrics |
| C++ Usage | `-cppusage` | Get C++ function calls |
| References | `-references` | Get asset dependencies |
| Graph | `-graph -depth=N` | Export dependency graph |
| Find Callers | `-func=Name -class=Class` | Find function callers |
| Native Events | `-nativeevents` | Find native event implementations |
| Output | `-out=file.json` | Write to file instead of stdout |
| JSON Mode | `-json` | Full JSON with nodes/connections |
| Skeleton Mode | `-skeleton` | C++ migration stubs |

## Grit Blueprint Locations

- `/Game/Showdown/UI/` - User interface widgets
- `/Game/Showdown/Blueprints/` - Game logic
- `/Game/Showdown/Characters/` - Character blueprints
- `/Game/Showdown/Modes/` - Game mode blueprints

## Development Guidelines

### Plugin Development

Source code is in `BlueprintAnalyzer/Source/`:
- Headers in `Public/` define data structures
- Implementation in `Private/`
- Commandlet handles CLI parsing and JSON output

### UE4.27 Compatibility Notes

- Use `FUNC_BlueprintEvent + FUNC_Native` to detect BlueprintNativeEvent (no separate flag in 4.27)
- Use `FindMetaData()` not `GetMetaData()` (returns pointer)
- Include `UObject/Script.h` for FUNC_* flags

### Testing

Test with Grit Blueprints:
```bash
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -path=/Game/Showdown/Modes/SDGameInstance_BP
```
