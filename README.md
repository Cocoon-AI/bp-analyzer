# Blueprint Analyzer

UE4.27 Editor plugin providing a CLI commandlet for Blueprint analysis and editing. Enables AI tools like Claude Code to inspect Blueprint structure, logic flow, and C++ function usage for debugging and migration assistance, and to mutate Blueprints via JSON-RPC.

## Features

- **Export Blueprints** in three formats: compact pseudocode, full JSON, or C++ migration skeleton
- **Analyze C++ usage** - find all C++ function calls within a Blueprint
- **Text search** - find-in-blueprints style search across node titles, comments, pin names/defaults, variable names
- **Search capabilities** - find Blueprints calling specific functions, implementing events, or with specific property values
- **Edit Blueprints** - add/remove/modify variables, functions, events, dispatchers, nodes, and components via JSON-RPC
- **Cleanup tools** - detect and remove broken-type nodes/variables, purge phantom properties from compiled classes
- **Dependency analysis** - export asset references, dependency graphs, and bidirectional reference viewer
- **Complexity metrics** - node counts, connection counts, and complexity scores
- **Persistent server mode** - keeps UE4 loaded for instant repeated queries via the `digbp` CLI

## Quick Start

### Using digbp (Recommended)

The `digbp` CLI communicates with a persistent UE4 server over named pipes, eliminating the ~30-60s startup on each query.

**1. Configure** — create `~/.digbp.yaml`:

```yaml
editor_cmd: "D:/Engine/Binaries/Win64/UE4Editor-Cmd.exe"
uproject: "D:/Projects/MyGame/MyGame.uproject"
```

**2. Build:**

```bash
cd bp-analyzer
go build -o digbp.exe ./cmd/digbp/
```

**3. Use** — the server auto-starts on first command:

```bash
# Export a Blueprint (JSON)
digbp export --path=/Game/Blueprints/MyBP --pretty

# Compact pseudocode
digbp export --path=/Game/Blueprints/MyBP --mode=compact

# C++ migration skeleton
digbp export --path=/Game/Blueprints/MyBP --mode=skeleton

# List Blueprints in a directory
digbp list --dir=/Game/UI/

# Find function callers
digbp findcallers --dir=/Game/ --func=GetPlayerController --class=UGameplayStatics
```

The first command launches the UE4 server (expect a startup delay). All subsequent commands are fast.

### Using the Commandlet Directly

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

**Git Bash/MSYS2**: Asset paths like `/Game/...` get mangled to local filesystem paths under MSYS, which makes both the commandlet and `digbp` return empty results. **Recommended:** set `MSYS_NO_PATHCONV=1` as a **Windows user environment variable** (Win+R → `sysdm.cpl` → Advanced → Environment Variables → User variables → New) so every shell inherits it. As a fallback, prefix individual commands inline: `MSYS_NO_PATHCONV=1 digbp export --path=/Game/...`. See [Troubleshooting → Path Mangling](#path-mangling-git-bash) for details.

## digbp CLI Reference

### Server Lifecycle

```bash
digbp start       # Launch UE4 server (auto-starts on first command too)
digbp status      # Check if server is running
digbp stop        # Shutdown server
```

### Commands

| Command | Description | Required Flags |
|---------|-------------|----------------|
| `digbp export` | Export a Blueprint | `--path` |
| `digbp list` | List Blueprints in directory | `--dir` |
| `digbp cppusage` | Get C++ function usage | `--path` |
| `digbp references` | Get asset references | `--path` |
| `digbp graph` | Export dependency graph | `--path` |
| `digbp refview` | Bidirectional reference viewer | `--path` |
| `digbp findcallers` | Find Blueprints calling a function | `--dir`, `--func` |
| `digbp nativeevents` | Find native event implementations | `--dir` |
| `digbp findevents` | Find implementable event implementations | `--dir`, `--event` |
| `digbp findprop` | Find Blueprints by CDO property | `--dir`, `--prop` |
| `digbp search` | Text search across Blueprints | `--dir`, `--query` |
| `digbp edit` | Mutate Blueprints (see below) | varies |

### Edit Commands

| Command | Description |
|---------|-------------|
| `digbp edit compile` | Compile a Blueprint |
| `digbp edit save` | Save a Blueprint to disk |
| `digbp edit save-and-compile` | Compile then save |
| `digbp edit variable list` | List variables (`--include-broken` for deleted types) |
| `digbp edit variable add` | Add a member variable |
| `digbp edit variable remove` | Remove a variable (`--force` for broken types) |
| `digbp edit variable rename` | Rename a variable |
| `digbp edit function add` | Add a function graph |
| `digbp edit function remove` | Remove a function |
| `digbp edit event add-custom` | Add a custom event |
| `digbp edit event remove` | Remove an event |
| `digbp edit dispatcher remove` | Remove an event dispatcher |
| `digbp edit node remove` | Remove a node by GUID |
| `digbp edit node remove-broken` | Remove all nodes with broken types (`--dry-run`) |
| `digbp edit purge-phantom` | Find/remove phantom properties + recompile (`--dry-run`) |
| `digbp edit component add` | Add a component |
| `digbp edit component remove` | Remove a component |

### Common Flags

| Flag | Description |
|------|-------------|
| `--pretty` | Pretty-print JSON output |
| `--path` | Blueprint asset path |
| `--dir` | Directory to search |
| `--mode` | Output mode: `json` (default), `compact`, `skeleton` |
| `--analyze` | Include complexity analysis (export only) |
| `--depth` | Graph traversal depth (default: 3) |
| `--refdepth` | Dependency depth for refview (default: 3) |
| `--referdepth` | Referencer depth for refview (default: 3) |
| `--bponly` | Only include Blueprints in refview |
| `--class` | Filter findcallers by class |
| `--value` | Filter findprop by property value |
| `--parentclass` | Filter findprop by parent class |
| `--no-recurse` | Don't recurse into subdirectories (list only) |

### Configuration

Create `~/.digbp.yaml`:

```yaml
editor_cmd: "D:/Engine/Binaries/Win64/UE4Editor-Cmd.exe"
uproject: "D:/Projects/MyGame/MyGame.uproject"
pipe_name: "blueprintexport"    # optional, default
start_timeout: 120s             # optional, default 120s
```

Environment variables override config: `DIGBP_EDITOR_CMD`, `DIGBP_UPROJECT`, `DIGBP_PIPE_NAME`.

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

## Commandlet Reference

### Basic Operations

| Flag | Description |
|------|-------------|
| `-path=/Game/Path/Blueprint` | Export single Blueprint |
| `-dir=/Game/Path/` | List Blueprints in directory |
| `-norecurse` | Don't search subdirectories (use with `-dir`) |
| `-out=file.json` | Write output to file instead of stdout |
| `-pipeserver` | Start persistent named pipe server |
| `-pipename=Name` | Custom pipe name (default: blueprintexport) |

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

## Architecture

### Server Mode (digbp)

```
AI Tool -> digbp CLI -> Named Pipe -> UE4 Server (stays running) -> JSON-RPC response
```

The `digbp` Go CLI manages a persistent UE4 process that keeps the editor and asset registry loaded. Communication uses Windows Named Pipes with JSON-RPC 2.0 over 4-byte length-prefix framing. The server handles one request at a time; all operations are serialized.

### CLI Mode (one-shot)

```
AI Tool -> Bash -> UE4Editor-Cmd.exe -run=BlueprintExport -> stdout
```

Output wrapped in `__JSON_START__...__JSON_END__` markers for reliable parsing. Extract JSON with:

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
│       │   ├── BlueprintExportData.h     # Data structures (14 USTRUCTs)
│       │   └── BlueprintExportReader.h   # Reader API
│       └── Private/
│           ├── BlueprintExportReader.cpp  # Core read/search implementation
│           ├── BlueprintExportCommandlet.cpp # CLI + output formatting
│           ├── BlueprintExportServer.cpp  # Named pipe server + JSON-RPC
│           ├── BlueprintEditOps_*.cpp     # Edit operations (variables, functions, nodes, etc.)
│           └── BlueprintAnalyzerModule.*
├── cmd/digbp/                      # Go CLI tool
│   ├── main.go                     # Root commands + search/find
│   └── edit*.go                    # Edit subcommands
├── internal/                       # Go internal packages
│   ├── config/config.go            # Config file loading
│   ├── pipe/client.go              # Named pipe client
│   ├── rpc/rpc.go                  # JSON-RPC 2.0 client
│   └── server/manager.go           # Server lifecycle management
├── claude-code-skills/             # Claude Code skills
│   ├── digbp/SKILL.md              # digbp skill (recommended)
│   └── blueprint-export/           # Direct commandlet skill (example)
├── go.mod
├── CLAUDE.md
└── README.md
```

## Troubleshooting

### Commandlet Not Found

- Ensure the plugin is installed correctly
- Rebuild the project after adding the plugin
- Check that `BlueprintAnalyzer` appears in the plugin list

### Path Mangling (Git Bash)

Git Bash / MSYS2 on Windows rewrites arguments that look like Unix paths into local filesystem paths. For example, `--path=/Game/Foo` becomes `--path=C:/Program Files/Git/Game/Foo`, which causes both the commandlet and `digbp` to return empty results or "blueprint not found" errors.

**Recommended fix — set it permanently:**

1. Win+R → `sysdm.cpl` → Advanced tab → Environment Variables
2. Under **User variables**, click New
3. Variable name: `MSYS_NO_PATHCONV`
4. Variable value: `1`
5. Restart Claude Code (and any other shells) so the new value is inherited

Quick check: `echo $MSYS_NO_PATHCONV` should print `1`.

**Inline fallback** if the env var is missing:

```bash
MSYS_NO_PATHCONV=1 digbp export --path=/Game/Blueprints/MyBP
MSYS_NO_PATHCONV=1 UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport -path=/Game/...
```

**Note:** `MSYS2_ARG_CONV_EXCL=/Game/;/Script/;/Engine/` does **not** work for `digbp`'s `--path=/Game/...` style arguments — the exclusion list matches the start of the entire argument (`--path=`), not the embedded path. Use `MSYS_NO_PATHCONV=1` instead.

### Blueprint Not Found

- Use exact asset paths (Copy Reference from Content Browser)
- Paths must start with `/Game/` or `/Engine/`
- Blueprint must be saved

### Server Won't Start

- Check `~/.digbp.yaml` paths are correct
- Verify UE4Editor-Cmd.exe path exists
- Verify .uproject path exists
- Check if another server is already running: `digbp status`
- Try starting manually: `digbp start` and watch stderr for errors

### Slow First Command

The first `digbp` command in a session launches the UE4 server, which takes 30-60s to load the asset registry. Subsequent commands are fast. Use `digbp start` ahead of time to pre-warm the server.

## License

MIT License
