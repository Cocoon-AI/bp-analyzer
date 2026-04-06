---
name: digbp
description: Analyze Unreal Engine Blueprints via the digbp persistent server. Use when the user asks to inspect, export, search, or understand Blueprints, find function callers, native events, asset references, or generate C++ migration stubs.
allowed-tools: Bash
argument-hint: "[command] [--flags]"
---

# digbp — Blueprint Analyzer CLI

`digbp` communicates with a persistent UE4 Blueprint Analyzer server over named pipes. The server stays running between commands, eliminating the ~60s UE4 startup cost on every query.

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

## Output

All commands return JSON to stdout. Use `--pretty` for indented output. Errors go to stderr.

When the user asks to analyze a blueprint by name without a full path, first use `digbp list` to find it:

```bash
digbp list --dir=/Game/ | jq '.blueprints[] | select(.name | test("PartialName"; "i"))'
```

## Resolving Partial Names

If the user gives a short name like "GameInstance" instead of a full path:
1. Use `digbp list --dir=/Game/ --pretty` and search the output for a matching name
2. Then use the full path from the result in subsequent commands

## Timeout

Use a Bash timeout of at least 120000ms for the first command in a session (server startup). Subsequent commands complete in seconds.
