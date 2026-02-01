# Blueprint Analyzer

UE4.27 Editor plugin providing a CLI commandlet for read-only Blueprint analysis. Enables AI tools like Claude Code to inspect Blueprint structure, logic flow, and C++ function usage for debugging and migration assistance.

## Features

- **Export Blueprints** in three formats: compact pseudocode, full JSON, or C++ migration skeleton
- **Analyze C++ usage** - find all C++ function calls within a Blueprint
- **Search capabilities** - find Blueprints calling specific functions, implementing events, or with specific property values
- **Dependency analysis** - export asset references and dependency graphs
- **Complexity metrics** - node counts, connection counts, and complexity scores

## Quick Start

```bash
# Export a Blueprint (compact pseudocode format)
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/Blueprints/MyBP

# Export as full JSON
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/Blueprints/MyBP -json

# Generate C++ migration skeleton
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/Blueprints/MyBP -skeleton

# Find all Blueprints calling a C++ function
UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -dir=/Game/ -func=GetPlayerController
```

**Git Bash/MSYS2**: Prefix with `MSYS_NO_PATHCONV=1` to prevent path mangling.

## Installation

### Option 1: Engine Plugin (Recommended)

Copy the `BlueprintAnalyzer` folder to your engine plugins directory:

```
BlueprintAnalyzer/ -> Engine/Plugins/Marketplace/BlueprintAnalyzer/
```

### Option 2: Project Plugin

Copy the `BlueprintAnalyzer` folder to your project's Plugins directory:

```
BlueprintAnalyzer/ -> YourProject/Plugins/BlueprintAnalyzer/
```

Then regenerate project files and rebuild.

## Output Modes

| Mode | Flag | Description |
|------|------|-------------|
| Compact | *(default)* | Pseudocode format - concise, readable logic flow |
| JSON | `-json` | Full JSON with all nodes, pins, and connections |
| Skeleton | `-skeleton` | C++ migration stubs with BP logic as comments |

### Compact Output Example

```
# Blueprint: MyGameMode_BP
# Path: /Game/Modes/MyGameMode_BP
# Parent: AGameModeBase

## Variables
  MaxPlayers: int32 = 16 [public]
  bGameStarted: bool [replicated]

## Functions
Function StartGame()
  IF bGameStarted
    return
  SET bGameStarted = true
  Call SpawnPlayers()  // C++

## Events
Event BeginPlay
  Call InitializeGame()
```

### Skeleton Output Example

```cpp
// Generated C++ skeleton for MyGameMode_BP
// Parent class: AGameModeBase

UCLASS()
class AMyGameMode_BP : public AGameModeBase
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintReadWrite)
    int32 MaxPlayers = 16;

    UPROPERTY(Replicated)
    bool bGameStarted;

    UFUNCTION(BlueprintCallable)
    void StartGame();
};

void AMyGameMode_BP::StartGame()
{
    // BP: Branch on bGameStarted
    // BP: Set bGameStarted = true
    // BP: SpawnPlayers() -> AGameModeBase::SpawnPlayers()
}
```

## Command Reference

### Basic Operations

| Flag | Description |
|------|-------------|
| `-path=/Game/Path/Blueprint` | Export single Blueprint |
| `-dir=/Game/Path/` | List Blueprints in directory |
| `-norecurse` | Don't search subdirectories (use with `-dir`) |
| `-out=file.json` | Write output to file instead of stdout |

### Output Modes

| Flag | Description |
|------|-------------|
| *(default)* | Compact pseudocode format |
| `-json` | Full JSON with nodes and connections |
| `-skeleton` | C++ migration stubs |

### Analysis Options

| Flag | Description |
|------|-------------|
| `-analyze` | Include complexity metrics (use with `-json`) |
| `-cppusage` | Get all C++ function calls in Blueprint |
| `-references` | Get all asset dependencies |
| `-graph -depth=N` | Export dependency graph (default depth: 3) |
| `-refview` | Reference viewer (bidirectional dependency graph) |
| `-refdepth=N` | Dependency depth for refview (default: 3) |
| `-referdepth=N` | Referencer depth for refview (default: 3) |
| `-bponly` | Only include Blueprints in refview |

### Search Operations

| Flags | Description |
|-------|-------------|
| `-dir=/Game/ -func=FunctionName` | Find Blueprints calling a function |
| `-dir=/Game/ -func=Name -class=Class` | Filter by function's class |
| `-dir=/Game/ -nativeevents` | Find BlueprintNativeEvent implementations |
| `-dir=/Game/ -event=EventName` | Find BlueprintImplementableEvent implementations |
| `-dir=/Game/ -findprop=PropertyName` | Find Blueprints with a CDO property |
| `-dir=/Game/ -findprop=Name -propvalue=Value` | Filter by property value |
| `-dir=/Game/ -findprop=Name -parentclass=Class` | Filter by parent class |

## Output Format

All output is wrapped in markers for reliable parsing:

```
__JSON_START__{"success":true,"blueprint_name":"MyBP",...}__JSON_END__
```

Extract JSON with:

```bash
... 2>&1 | sed -n 's/.*__JSON_START__\(.*\)__JSON_END__.*/\1/p'
```

## Data Extracted

| Data | Description |
|------|-------------|
| Variables | Name, type, default value, visibility, replication settings |
| Functions | Name, inputs, outputs, nodes, connections, pure/static/const flags |
| Events | Event handlers with full node graphs |
| Components | Scene component hierarchy with transforms |
| References | Hard and soft asset dependencies |
| C++ Usage | All C++ function calls with BlueprintCallable/NativeEvent flags |

## Project Structure

```
bp-analyzer/
├── BlueprintAnalyzer/              # UE4 Plugin
│   ├── BlueprintAnalyzer.uplugin
│   └── Source/BlueprintAnalyzer/
│       ├── Public/
│       │   ├── BlueprintExportData.h   # Data structures
│       │   └── BlueprintExportReader.h # Reader API
│       └── Private/
│           ├── BlueprintExportReader.cpp
│           ├── BlueprintExportCommandlet.cpp
│           └── BlueprintAnalyzerModule.*
├── claude-code-skills/             # Claude Code skill
│   └── blueprint-export/SKILL.md
├── CLAUDE.md                       # AI assistant instructions
└── README.md
```

## Troubleshooting

### Commandlet Not Found

- Ensure the plugin is installed correctly
- Rebuild the project after adding the plugin
- Check that `BlueprintAnalyzer` appears in the plugin list

### Path Mangling (Git Bash)

```bash
MSYS_NO_PATHCONV=1 UE4Editor-Cmd.exe ... -path=/Game/...
```

### Blueprint Not Found

- Use exact asset paths (Copy Reference from Content Browser)
- Paths must start with `/Game/` or `/Engine/`
- Blueprint must be saved

### Slow Startup

The commandlet requires loading the asset registry. First invocation may take 10-30 seconds depending on project size.

## License

MIT License
