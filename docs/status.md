# MCP Unreal Blueprint Server - Status

## Current Architecture

### Commandlet-Based Invocation (NEW)
The project now uses a **commandlet-based architecture** instead of the previous TCP socket approach:

```
Claude Code → Bash → UE4Editor-Cmd.exe -run=BlueprintExport → JSON output
```

**Advantages:**
- No need for running Unreal Editor UI
- No TCP socket communication required
- Direct CLI invocation with JSON output
- Can be called as a Claude Code skill

### Components

| Component | Status | Description |
|-----------|--------|-------------|
| **BlueprintAnalyzer Plugin** | ✅ Complete | UE4.27 plugin with commandlet |
| **MCP Server (Python)** | ⚠️ Legacy | Original TCP-based server (still works) |
| **Commandlet** | ✅ Complete | CLI tool for direct invocation |

## Project Timeline

### Phase 1: Foundation ✅
- [x] Create project structure
- [x] Define objectives and requirements
- [x] Research Unreal Python API capabilities
- [x] Set up basic MCP server structure

### Phase 2: Core Functionality ✅
- [x] Implement Blueprint asset loading
- [x] Extract Blueprint node information
- [x] Parse Blueprint connections and variables
- [x] Create data structures for Blueprint representation
- [x] Integrate BlueprintAnalyzer plugin

### Phase 3: Platform Refocus ✅
- [x] Migrate from WSL to native Windows development
- [x] Focus on UE 4.27 (Grit project)
- [x] Update documentation for Windows environment
- [x] Clarify read-only scope (no Blueprint creation)

### Phase 4: Commandlet Architecture ✅
- [x] Recover lost plugin source from UHT-generated files
- [x] Rewrite BlueprintExportReader implementation
- [x] Create BlueprintExportCommandlet for CLI invocation
- [x] Apply UE4.27 compatibility fixes
- [x] Successfully build and test commandlet

### Phase 5: Integration & Polish (Current)
- [ ] Create Claude Code skill for commandlet invocation
- [ ] Add comprehensive error handling
- [ ] Optimize for large Blueprint analysis
- [ ] Document all commandlet options

### Phase 6: C++ Migration Support (Future)
- [ ] Analyze Blueprint complexity for migration candidates
- [ ] Generate C++ code suggestions from Blueprint logic
- [ ] Track Blueprint-to-C++ function mappings

## Commandlet Operations

| Operation | Command | Description |
|-----------|---------|-------------|
| Export Blueprint | `-path=/Game/...` | Export single blueprint to JSON |
| Analyze | `-path=... -analyze` | Include complexity analysis |
| Find Blueprints | `-dir=/Game/...` | List blueprints in directory |
| C++ Usage | `-path=... -cppusage` | Get C++ function calls |
| References | `-path=... -references` | Get asset dependencies |
| Dependency Graph | `-path=... -graph -depth=N` | Export dependency graph |
| Find Callers | `-dir=... -func=Name -class=Class` | Find function callers |
| Native Events | `-dir=... -nativeevents` | Find native event implementations |

## Current Status
- **Last Updated**: January 17, 2026
- **Current Phase**: Phase 5 - Integration & Polish
- **Platform**: Windows (native), UE 4.27
- **Target Project**: Grit (D:\sd\dev)
- **Plugin Location**: D:\sd\dev\Engine\Plugins\Marketplace\BlueprintAnalyzer\

## Files

```
mcp-unrealbp/
├── BlueprintAnalyzer/           # UE4 Plugin (ready to install)
│   ├── BlueprintAnalyzer.uplugin
│   └── Source/BlueprintAnalyzer/
│       ├── BlueprintAnalyzer.Build.cs
│       ├── Public/
│       │   ├── BlueprintExportData.h
│       │   └── BlueprintExportReader.h
│       └── Private/
│           ├── BlueprintExportReader.cpp
│           ├── BlueprintExportCommandlet.h
│           ├── BlueprintExportCommandlet.cpp
│           ├── BlueprintAnalyzerModule.h
│           └── BlueprintAnalyzerModule.cpp
├── src/mcp_unrealbp/                # Legacy MCP server (Python)
├── docs/
│   ├── plan.md                      # Project objectives
│   └── status.md                    # This file
└── README.md                        # Installation & usage
```
