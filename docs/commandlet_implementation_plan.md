# Blueprint Export Commandlet Implementation Plan

## Overview

Replace the current TCP-based MCP server architecture with a direct commandlet invocation that Claude Code can call via Bash. This eliminates the need for a running Unreal Editor and provides faster, more reliable Blueprint analysis.

## Current Architecture (Problems)

```
Claude Code → MCP Server → TCP Socket → Unreal Editor (running) → Python → Plugin
```

**Issues:**
- Requires Unreal Editor to be running
- Manual startup of mcp_server.py in Unreal
- TCP protocol has buffer size limitations
- WSL/Windows path complexity
- ~30+ second latency for editor startup

## New Architecture (Proposed)

```
Claude Code → Bash → UE4Editor-Cmd.exe -run=BlueprintExport → JSON to stdout
```

**Benefits:**
- No running editor required
- Direct invocation via Bash tool
- Fast commandlet startup (~5-10 seconds)
- Output directly to stdout for parsing
- Can be wrapped as a Claude Code skill

## Implementation

### 1. Create New Editor Module: `ShowdownBlueprintTools`

**Location:** `D:\sd\dev\Showdown\Source\ShowdownBlueprintTools\`

**Files to create:**

#### ShowdownBlueprintTools.Build.cs
```csharp
using UnrealBuildTool;

public class ShowdownBlueprintTools : ModuleRules
{
    public ShowdownBlueprintTools(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "BlueprintAnalyzer",  // The marketplace plugin
            "Json",
            "JsonUtilities"
        });

        if (Target.Type == TargetType.Editor)
        {
            PublicDependencyModuleNames.Add("UnrealEd");
        }
    }
}
```

#### ShowdownBlueprintToolsModule.h
```cpp
#pragma once

#include "Modules/ModuleManager.h"

class FShowdownBlueprintToolsModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
```

#### ShowdownBlueprintToolsModule.cpp
```cpp
#include "ShowdownBlueprintToolsModule.h"

IMPLEMENT_MODULE(FShowdownBlueprintToolsModule, ShowdownBlueprintTools)

void FShowdownBlueprintToolsModule::StartupModule() {}
void FShowdownBlueprintToolsModule::ShutdownModule() {}
```

#### BlueprintExportCommandlet.h
```cpp
#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BlueprintExportCommandlet.generated.h"

UCLASS()
class UBlueprintExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UBlueprintExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    // Export single blueprint
    void ExportBlueprint(const FString& BlueprintPath, bool bAnalyze);

    // Find blueprints in directory
    void FindBlueprints(const FString& DirectoryPath, bool bRecursive);

    // Find blueprints calling a C++ function
    void FindBlueprintsCallingFunction(const FString& FunctionName,
                                        const FString& ClassName,
                                        const TArray<FString>& SearchPaths);

    // Get C++ usage for a blueprint
    void GetCppUsage(const FString& BlueprintPath);

    // Output JSON to stdout
    void OutputJson(const TSharedPtr<FJsonObject>& JsonObject);

    // Output error
    void OutputError(const FString& Error);
};
```

#### BlueprintExportCommandlet.cpp
```cpp
#include "BlueprintExportCommandlet.h"
#include "BlueprintExportReader.h"  // From BlueprintAnalyzer plugin
#include "BlueprintExportData.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/CommandLine.h"

UBlueprintExportCommandlet::UBlueprintExportCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;  // Need editor for Blueprint access
    LogToConsole = false;  // We control output
    ShowErrorCount = false;
}

int32 UBlueprintExportCommandlet::Main(const FString& Params)
{
    TArray<FString> Tokens;
    TArray<FString> Switches;
    TMap<FString, FString> ParamsMap;
    ParseCommandLine(*Params, Tokens, Switches, ParamsMap);

    // Determine operation mode
    FString BlueprintPath;
    FString DirectoryPath;
    FString FunctionName;
    FString ClassName;
    bool bAnalyze = Switches.Contains(TEXT("analyze"));
    bool bRecursive = !Switches.Contains(TEXT("norecurse"));
    bool bCppUsage = Switches.Contains(TEXT("cppusage"));

    if (ParamsMap.Contains(TEXT("path")))
    {
        BlueprintPath = ParamsMap[TEXT("path")];
    }
    if (ParamsMap.Contains(TEXT("dir")))
    {
        DirectoryPath = ParamsMap[TEXT("dir")];
    }
    if (ParamsMap.Contains(TEXT("func")))
    {
        FunctionName = ParamsMap[TEXT("func")];
    }
    if (ParamsMap.Contains(TEXT("class")))
    {
        ClassName = ParamsMap[TEXT("class")];
    }

    // Execute appropriate operation
    if (!BlueprintPath.IsEmpty())
    {
        if (bCppUsage)
        {
            GetCppUsage(BlueprintPath);
        }
        else
        {
            ExportBlueprint(BlueprintPath, bAnalyze);
        }
    }
    else if (!DirectoryPath.IsEmpty())
    {
        if (!FunctionName.IsEmpty())
        {
            TArray<FString> SearchPaths;
            SearchPaths.Add(DirectoryPath);
            FindBlueprintsCallingFunction(FunctionName, ClassName, SearchPaths);
        }
        else
        {
            FindBlueprints(DirectoryPath, bRecursive);
        }
    }
    else
    {
        OutputError(TEXT("No operation specified. Use -path=/Game/BP or -dir=/Game/"));
        return 1;
    }

    return 0;
}

void UBlueprintExportCommandlet::ExportBlueprint(const FString& BlueprintPath, bool bAnalyze)
{
    UBlueprintExportReader* Reader = NewObject<UBlueprintExportReader>();
    FBlueprintExportData ExportData = Reader->ExportBlueprint(BlueprintPath);

    if (ExportData.BlueprintName.IsEmpty())
    {
        OutputError(FString::Printf(TEXT("Failed to load blueprint: %s"), *BlueprintPath));
        return;
    }

    // Convert to JSON
    TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
    JsonObject->SetStringField(TEXT("blueprint_name"), ExportData.BlueprintName);
    JsonObject->SetStringField(TEXT("blueprint_path"), ExportData.BlueprintPath);
    JsonObject->SetStringField(TEXT("parent_class"), ExportData.ParentClass);
    JsonObject->SetStringField(TEXT("blueprint_type"), ExportData.BlueprintType);
    // ... convert all fields ...

    OutputJson(JsonObject);
}

void UBlueprintExportCommandlet::OutputJson(const TSharedPtr<FJsonObject>& JsonObject)
{
    FString OutputString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
    FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

    // Output to stdout
    UE_LOG(LogTemp, Display, TEXT("__JSON_START__%s__JSON_END__"), *OutputString);
}

void UBlueprintExportCommandlet::OutputError(const FString& Error)
{
    TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
    JsonObject->SetBoolField(TEXT("success"), false);
    JsonObject->SetStringField(TEXT("error"), Error);
    OutputJson(JsonObject);
}

// ... implement other methods ...
```

### 2. Update ShowdownEditor.Target.cs

Add the new module to editor builds:

```csharp
ExtraModuleNames.Add("ShowdownBlueprintTools");
```

### 3. Update Showdown.uproject

Add module dependency:

```json
{
    "Name": "ShowdownBlueprintTools",
    "Type": "Editor",
    "LoadingPhase": "Default"
}
```

## Usage Examples

### Export Single Blueprint
```bash
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -path=/Game/Showdown/UI/MainMenu
```

### Export with Analysis
```bash
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -path=/Game/Showdown/UI/MainMenu -analyze
```

### Find All Blueprints in Directory
```bash
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -dir=/Game/Showdown/UI/ -norecurse
```

### Find Blueprints Calling C++ Function
```bash
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -dir=/Game/ -func=GetPlayerController -class=UGameplayStatics
```

### Get C++ Usage for Blueprint
```bash
UE4Editor-Cmd.exe "D:/sd/dev/Showdown/Showdown.uproject" -run=BlueprintExport -path=/Game/Showdown/Characters/Player -cppusage
```

## Claude Code Skill Integration

Create a `/blueprint` skill that wraps these commands:

```python
# skills/blueprint.py

COMMANDS = {
    "export": "-path={path}",
    "analyze": "-path={path} -analyze",
    "find": "-dir={dir}",
    "callers": "-dir=/Game/ -func={function}",
    "cpp-usage": "-path={path} -cppusage"
}

def run_blueprint_command(subcommand: str, **kwargs) -> dict:
    base_cmd = 'D:/sd/dev/Engine/Binaries/Win64/UE4Editor-Cmd.exe'
    project = 'D:/sd/dev/Showdown/Showdown.uproject'

    cmd = f'{base_cmd} "{project}" -run=BlueprintExport {COMMANDS[subcommand].format(**kwargs)}'

    # Execute and parse JSON from output
    result = subprocess.run(cmd, capture_output=True, text=True)

    # Extract JSON between markers
    match = re.search(r'__JSON_START__(.+?)__JSON_END__', result.stdout, re.DOTALL)
    if match:
        return json.loads(match.group(1))

    return {"error": "Failed to parse output"}
```

## Migration Path

1. **Phase 1:** Create the commandlet module and test manually
2. **Phase 2:** Create Claude Code skill wrapper
3. **Phase 3:** Deprecate TCP-based MCP server
4. **Phase 4:** Remove old mcp_server.py and related code

## Considerations

### Startup Time
Commandlet startup is faster than full editor but still requires asset registry.
Expected: 5-15 seconds depending on project size.

### Output Size
Large blueprints can produce significant JSON. Consider:
- Pagination for large results
- File output option (`-out=file.json`)
- Compression for very large exports

### Error Handling
All errors should be JSON formatted with:
```json
{
    "success": false,
    "error": "Human readable message",
    "code": "ERROR_CODE"
}
```

### Caching
Consider implementing a caching layer:
- Cache blueprint exports by path + modification time
- Store in temp directory
- Invalidate on blueprint changes
