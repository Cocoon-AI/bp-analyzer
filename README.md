# Blueprint Analyzer

UE4 plugin with CLI commandlet for analyzing Unreal Engine Blueprints. Enables tools like Claude Code to read, analyze, and understand Blueprint structure for debugging and C++ migration assistance.

**Target**: Unreal Engine 4.27 | **Focus**: Read-only Blueprint analysis

## Quick Start

Direct CLI invocation with JSON output - no running editor required:

```bash
# Export a Blueprint (compact pseudocode format)
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -path=/Game/Showdown/UI/MainMenu

# Export as full JSON
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -json

# Generate C++ migration skeleton
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -skeleton

# Get C++ function usage
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/Characters/Player -cppusage

# Find all Blueprints in a directory
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/Showdown/UI/
```

**Note for Git Bash/MSYS2**: Prefix with `MSYS_NO_PATHCONV=1` to prevent path mangling.

## Installation

### Option 1: Install Plugin to Engine (Recommended)

Copy the `BlueprintAnalyzer` folder to your engine plugins directory:

```
BlueprintAnalyzer/ → D:\sd\dev\Engine\Plugins\Marketplace\BlueprintAnalyzer\
```

Then regenerate project files and rebuild:

```cmd
D:\sd\dev\Engine\Binaries\DotNET\UnrealBuildTool.exe -projectfiles -project="D:\sd\dev\Showdown\Showdown.uproject"
```

### Option 2: Install to Project

Copy the `BlueprintAnalyzer` folder to your project's Plugins directory:

```
BlueprintAnalyzer/ → YourProject/Plugins/BlueprintAnalyzer/
```

## Output Modes

The commandlet supports three output formats:

| Mode | Flag | Description |
|------|------|-------------|
| Compact | *(default)* | Pseudocode format - concise, readable logic flow |
| JSON | `-json` | Full JSON with all nodes, pins, and connections |
| Skeleton | `-skeleton` | C++ migration stubs with BP logic as comments |

### Compact Output Example

```
Blueprint: SDGameInstance_BP
Parent: SDGameInstance
Type: Blueprint

Variables:
  - bShowMainMenu: bool = true
  - CurrentGameState: EGameState

Functions:
  InitializeGame():
    SetGameState(NewState=Playing)
    SpawnPlayers()
```

### Skeleton Output Example

```cpp
// Generated C++ skeleton for SDGameInstance_BP
UCLASS()
class USDGameInstance_BP : public USDGameInstance
{
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadWrite)
    bool bShowMainMenu = true;

    UFUNCTION(BlueprintCallable)
    void InitializeGame();
};
```

## Commandlet Reference

### Basic Operations

| Command | Description |
|---------|-------------|
| `-path=/Game/Path/Blueprint` | Export single Blueprint |
| `-dir=/Game/Path/` | List Blueprints in directory |
| `-out=file.json` | Write output to file instead of stdout |
| `-norecurse` | Don't search subdirectories |

### Output Modes

| Flag | Description |
|------|-------------|
| *(default)* | Compact pseudocode format |
| `-json` | Full JSON with nodes and connections |
| `-skeleton` | C++ migration stubs |

### Analysis Options

| Flag | Description |
|------|-------------|
| `-analyze` | Include complexity analysis (use with `-json`) |
| `-cppusage` | Get all C++ function calls in Blueprint |
| `-references` | Get all asset dependencies |
| `-graph -depth=N` | Export dependency graph (default depth: 3) |

### Search Operations

| Command | Description |
|---------|-------------|
| `-dir=/Game/ -func=FunctionName` | Find Blueprints calling a function |
| `-dir=/Game/ -func=Name -class=ClassName` | Filter by function's class |
| `-dir=/Game/ -nativeevents` | Find BlueprintNativeEvent implementations |

### Examples

```bash
# Export with complexity analysis
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/BP -json -analyze

# Find all Blueprints calling GetPlayerController
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/ -func=GetPlayerController -class=UGameplayStatics

# Export dependency graph with depth 5
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/MainMenu -graph -depth=5

# Get references for a Blueprint
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/Characters/Player -references

# Find native event implementations
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/Showdown/ -nativeevents
```

### Output Format

JSON output is wrapped in markers for reliable parsing:

```
__JSON_START__{"success":true,"blueprint_name":"MyBlueprint",...}__JSON_END__
```

## Data Structures

The plugin exports comprehensive Blueprint data:

| Structure | Contents |
|-----------|----------|
| `FBlueprintExportData` | Complete Blueprint: name, path, parent, type, variables, functions, events, components |
| `FBlueprintNodeData` | Node info: GUID, type, title, pins, position, C++ function metadata |
| `FBlueprintConnectionData` | Graph connections between nodes |
| `FBlueprintVariableData` | Variables with type, default value, replication settings |
| `FBlueprintFunctionData` | Functions with inputs, outputs, nodes, connections |
| `FBlueprintCppFunctionUsage` | C++ function call tracking with BlueprintCallable/NativeEvent flags |

## Project Structure

```
bp-analyzer/
├── BlueprintAnalyzer/              # UE4 Plugin (ready to install)
│   ├── BlueprintAnalyzer.uplugin
│   └── Source/BlueprintAnalyzer/
│       ├── BlueprintAnalyzer.Build.cs
│       ├── Public/
│       │   ├── BlueprintExportData.h   # All data structures
│       │   └── BlueprintExportReader.h # Reader class
│       └── Private/
│           ├── BlueprintExportReader.cpp    # Implementation
│           ├── BlueprintExportCommandlet.h  # CLI commandlet
│           ├── BlueprintExportCommandlet.cpp
│           └── BlueprintAnalyzerModule.*
├── claude-code-skills/             # Claude Code skill definitions
│   └── blueprint-export/SKILL.md
├── docs/
│   ├── plan.md                     # Project objectives
│   └── status.md                   # Current status
└── README.md
```

## Use Cases

1. **Blueprint Debugging**: Analyze structure, find broken connections, trace execution flow
2. **C++ Migration**: Generate skeleton code, identify C++ function usage, find migration candidates
3. **Codebase Understanding**: Map Blueprint dependencies, find function callers
4. **Performance Analysis**: Complexity metrics, node counts, identify optimization opportunities

## Troubleshooting

### Commandlet Not Found
- Ensure the plugin is installed and the project is rebuilt
- Verify the commandlet is registered (check build output)

### Path Mangling (Git Bash)
```bash
MSYS_NO_PATHCONV=1 UE4Editor-Cmd.exe ... -path=/Game/...
```

### Blueprint Not Found
- Use exact asset paths (Copy Reference from Content Browser)
- Paths must start with `/Game/` or `/Engine/`
- Blueprint must be saved

## License

MIT License
