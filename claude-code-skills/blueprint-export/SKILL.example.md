<!--
  SKILL.example.md - Example Claude Code Skill for Blueprint Analyzer

  This is an EXAMPLE skill file. To use it:
  1. Copy this file to your project's .claude/skills/blueprint-export/SKILL.md
  2. Update all paths to match your project (engine path, .uproject path, content paths)
  3. Customize the "Key Blueprint Paths" and "Common Directories" sections for your project

  Learn more about Claude Code skills:
  https://docs.anthropic.com/en/docs/claude-code/skills
-->
---
name: blueprint-export
description: Export and analyze Unreal Engine Blueprint data. Use when the user asks to analyze, export, inspect, or understand blueprints, find which blueprints call a C++ function, or generate C++ migration stubs.
allowed-tools: Bash
---

# Blueprint Export Tool

Export and analyze Unreal Engine Blueprint data using the BlueprintExport commandlet.

## Quick Start

Export a blueprint:

```bash
# UPDATE THESE PATHS for your project:
# - Engine path: Path to your UE4Editor-Cmd.exe
# - Project path: Path to your .uproject file
# - Blueprint path: Path to a blueprint in your project
MSYS_NO_PATHCONV=1 "Path/To/Engine/Binaries/Win64/UE4Editor-Cmd.exe" \
  "Path/To/YourProject.uproject" \
  -run=BlueprintExport \
  -path=/Game/YourProject/Blueprints/ExampleBP
```

**IMPORTANT**: Always use `MSYS_NO_PATHCONV=1` to prevent Git bash from mangling `/Game/...` paths.

## Path Variables

The examples below use these placeholder variables - replace with your actual paths:

```bash
# Example values - UPDATE FOR YOUR PROJECT:
UE4_EDITOR_CMD="D:/Engine/Binaries/Win64/UE4Editor-Cmd.exe"
PROJECT="D:/Projects/MyGame/MyGame.uproject"
```

## Output Modes

The commandlet supports three output formats (mutually exclusive):

| Mode | Flag | Description |
|------|------|-------------|
| Compact | *(default)* | Pseudocode format - concise, readable logic flow |
| JSON | `-json` | Full JSON with all nodes, pins, and connections |
| Skeleton | `-skeleton` | C++ migration stubs with BP logic as comments |

## Available Options

### Path/Directory (mutually exclusive)
| Option | Description |
|--------|-------------|
| `-path=/Game/Path/To/BP` | Export a single blueprint |
| `-dir=/Game/Path/` | Find all blueprints in directory |

### Search & Filter
| Option | Description |
|--------|-------------|
| `-func=FunctionName` | Find blueprints calling this function (use with `-dir`) |
| `-class=ClassName` | Filter function search by class (use with `-func`) |
| `-findprop=PropertyName` | Find blueprints with this CDO property (use with `-dir`) |
| `-propvalue=Value` | Filter by property value (use with `-findprop`) |
| `-parentclass=ClassName` | Filter by parent class (use with `-findprop`) |
| `-event=EventName` | Find BlueprintImplementableEvent implementations (use with `-dir`) |
| `-norecurse` | Don't search subdirectories (use with `-dir`) |

### Analysis
| Option | Description |
|--------|-------------|
| `-analyze` | Include complexity metrics (use with `-json`) |
| `-cppusage` | Get C++ function usage for blueprint |
| `-references` | Get all asset references from blueprint |
| `-graph` | Export full dependency graph |
| `-depth=N` | Maximum graph depth (default 3, use with `-graph`) |
| `-nativeevents` | Find native event implementations (use with `-dir`) |

### Output
| Option | Description |
|--------|-------------|
| `-out=output.json` | Write output to file instead of stdout |
| `-json` | Full JSON output with nodes and connections |
| `-skeleton` | C++ migration stubs output |

## Common Tasks

### Export a Single Blueprint (Compact)

```bash
MSYS_NO_PATHCONV=1 "$UE4_EDITOR_CMD" "$PROJECT" -run=BlueprintExport -path=$ARGUMENTS
```

### Export Blueprint as Full JSON

```bash
MSYS_NO_PATHCONV=1 "$UE4_EDITOR_CMD" "$PROJECT" -run=BlueprintExport -path=$ARGUMENTS -json
```

### Generate C++ Migration Skeleton

```bash
MSYS_NO_PATHCONV=1 "$UE4_EDITOR_CMD" "$PROJECT" -run=BlueprintExport -path=$ARGUMENTS -skeleton
```

### Get C++ Function Usage

```bash
MSYS_NO_PATHCONV=1 "$UE4_EDITOR_CMD" "$PROJECT" -run=BlueprintExport -path=$ARGUMENTS -cppusage
```

### Find Blueprints Calling a C++ Function

```bash
MSYS_NO_PATHCONV=1 "$UE4_EDITOR_CMD" "$PROJECT" -run=BlueprintExport -dir=/Game/ -func=FunctionName -class=ClassName
```

### List Blueprints in Directory

```bash
MSYS_NO_PATHCONV=1 "$UE4_EDITOR_CMD" "$PROJECT" -run=BlueprintExport -dir=/Game/UI/
```

### Find Native Event Implementations

```bash
MSYS_NO_PATHCONV=1 "$UE4_EDITOR_CMD" "$PROJECT" -run=BlueprintExport -dir=/Game/ -nativeevents
```

### Find Implementable Event Implementations

Find blueprints that implement a specific `BlueprintImplementableEvent`.

```bash
MSYS_NO_PATHCONV=1 "$UE4_EDITOR_CMD" "$PROJECT" -run=BlueprintExport -dir=/Game/ -event=ReceiveBeginPlay
```

### Find Blueprints by CDO Property Value

Find all blueprints where a specific Class Default Object (CDO) property has a certain value.
This is useful for finding Blueprint classes that override C++ base class property values.

```bash
# Find blueprints with a specific property value
MSYS_NO_PATHCONV=1 "$UE4_EDITOR_CMD" "$PROJECT" -run=BlueprintExport -dir=/Game/ -findprop=bCanBeDamaged -propvalue=true

# Find blueprints derived from a specific class with a property
MSYS_NO_PATHCONV=1 "$UE4_EDITOR_CMD" "$PROJECT" -run=BlueprintExport -dir=/Game/ -findprop=bCanBeDamaged -parentclass=APawn
```

### Get Asset References

```bash
MSYS_NO_PATHCONV=1 "$UE4_EDITOR_CMD" "$PROJECT" -run=BlueprintExport -path=$ARGUMENTS -references
```

### Export Dependency Graph

```bash
MSYS_NO_PATHCONV=1 "$UE4_EDITOR_CMD" "$PROJECT" -run=BlueprintExport -path=$ARGUMENTS -graph -depth=5
```

## Output Format

Output is wrapped in markers for reliable parsing:

```
__JSON_START__{"success":true,...}__JSON_END__
```

Extract JSON with:

```bash
... 2>&1 | sed -n '/__JSON_START__/,/__JSON_END__/p' | sed 's/__JSON_START__//;s/__JSON_END__//'
```

### Compact Output Example

```
Blueprint: MyGameMode_BP
Parent: AGameModeBase
Type: Blueprint

Variables:
  - MaxPlayers: int32 = 16
  - bGameStarted: bool

Functions:
  StartGame():
    SetGameState(NewState=Playing)
    SpawnPlayers()

Events:
  BeginPlay:
    InitializeGame()
```

### JSON Output Example

```json
{
  "success": true,
  "blueprint_name": "MyGameMode_BP",
  "blueprint_path": "/Game/Blueprints/MyGameMode_BP",
  "parent_class": "AGameModeBase",
  "blueprint_type": "Blueprint",
  "variables": [...],
  "functions": [...],
  "event_graph": [...],
  "components": [],
  "analysis": {
    "total_functions": 5,
    "total_variables": 12,
    "total_events": 3,
    "total_nodes": 47,
    "complexity_score": 23
  }
}
```

### Skeleton Output Example

```cpp
// Generated C++ skeleton for MyGameMode_BP
// Parent class: AGameModeBase

UCLASS()
class AMyGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintReadWrite)
    int32 MaxPlayers = 16;

    UFUNCTION(BlueprintCallable)
    void StartGame();
};

void AMyGameMode::StartGame()
{
    // BP Logic:
    // SetGameState(NewState=Playing)
    // SpawnPlayers()
}
```

## Key Blueprint Paths

<!-- UPDATE: Add your project's important blueprints here -->
| Blueprint | Path |
|-----------|------|
| Game Mode | `/Game/Blueprints/MyGameMode_BP` |
| Player Character | `/Game/Characters/PlayerCharacter_BP` |
| Main Menu | `/Game/UI/MainMenu_WBP` |

## Common Directories

<!-- UPDATE: Add your project's common blueprint directories here -->
- `/Game/Blueprints/` - Core game logic
- `/Game/UI/` - User interface widgets
- `/Game/Characters/` - Character blueprints

## Timeout

Use a timeout of at least 120000ms (2 minutes) as the commandlet takes time to initialize.

## Resolving Blueprint Names

<!-- UPDATE: Add your project's common directories for name resolution -->
If the user provides a partial name, search in common locations:
- `/Game/Blueprints/`
- `/Game/UI/`
- `/Game/Characters/`
