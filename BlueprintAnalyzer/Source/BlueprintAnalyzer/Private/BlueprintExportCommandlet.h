// BlueprintExportCommandlet.h
// Commandlet for CLI invocation of Blueprint analysis

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BlueprintExportCommandlet.generated.h"

// Forward declaration for server mode
class FBlueprintExportServer;

// Output format modes
enum class EBlueprintExportMode : uint8
{
	Compact,   // Pseudocode format (default)
	Json,      // Full JSON with nodes/connections
	Skeleton   // C++ migration stubs
};

/**
 * Commandlet for exporting Blueprint data via command line
 *
 * Usage:
 *   UE4Editor-Cmd.exe "Project.uproject" -run=BlueprintExport [options]
 *
 * Options:
 *   -path=/Game/Path/To/Blueprint   Export a single blueprint
 *   -dir=/Game/Path/                Find all blueprints in directory
 *   -func=FunctionName              Find blueprints calling this function
 *   -class=ClassName                Filter function search by class
 *   -out=output.json                Write output to file instead of stdout
 *   -analyze                        Include complexity analysis
 *   -cppusage                       Get C++ function usage for blueprint
 *   -references                     Get all references from blueprint
 *   -norecurse                      Don't search subdirectories
 *   -graph                          Export full dependency graph
 *   -depth=N                        Maximum graph depth (default 3)
 *   -findprop=PropertyName          Find blueprints with this CDO property
 *   -propvalue=Value                Filter by property value (optional)
 *   -parentclass=ClassName          Filter by parent class (optional)
 *   -refview                        Reference viewer mode (bidirectional graph)
 *   -refdepth=N                     Depth for dependencies in refview (default 3)
 *   -referdepth=N                   Depth for referencers in refview (default 3)
 *   -bponly                         Only include Blueprint assets in refview
 *   -pipeserver                     Run in persistent server mode (named pipe)
 *   -pipename=Name                  Named pipe name (default: blueprintexport)
 *
 * Output Modes (mutually exclusive):
 *   (default)                       Compact pseudocode format for analysis
 *   -json                           Full JSON with nodes and connections
 *   -skeleton                       C++ migration stubs with BP logic as comments
 */
UCLASS()
class BLUEPRINTANALYZER_API UBlueprintExportCommandlet : public UCommandlet
{
	GENERATED_BODY()

	friend class FBlueprintExportServer;

public:
	UBlueprintExportCommandlet();

	virtual int32 Main(const FString& Params) override;

	// --- JSON-returning operation methods (used by both CLI and server mode) ---

	// Export single blueprint as JSON
	TSharedPtr<FJsonObject> ExportBlueprintToJson(const FString& BlueprintPath, bool bAnalyze);

	// Export single blueprint as text (compact or skeleton). When bAnalyze is
	// true, the output is prefixed with a comment-style analysis header.
	FString ExportBlueprintToText(const FString& BlueprintPath, EBlueprintExportMode Mode, bool bAnalyze = false);

	// List blueprints in directory
	TSharedPtr<FJsonObject> ExportDirectoryToJson(const FString& DirectoryPath, bool bRecursive);

	// Get C++ function usage
	TSharedPtr<FJsonObject> GetCppUsageToJson(const FString& BlueprintPath);

	// Get blueprint references
	TSharedPtr<FJsonObject> GetReferencesToJson(const FString& BlueprintPath);

	// Export dependency graph
	TSharedPtr<FJsonObject> ExportGraphToJson(const FString& RootPath, int32 MaxDepth);

	// Find blueprints calling specific function
	TSharedPtr<FJsonObject> FindCallersToJson(const FString& FunctionName, const FString& ClassName, const TArray<FString>& SearchPaths);

	// Find blueprints reading/writing a named variable
	TSharedPtr<FJsonObject> FindVarUsesToJson(const FString& VariableName, const FString& Kind, const TArray<FString>& SearchPaths);

	// Find native event implementations
	TSharedPtr<FJsonObject> FindNativeEventsToJson(const TArray<FString>& SearchPaths);

	// Find implementable event implementations
	TSharedPtr<FJsonObject> FindImplementableEventsToJson(const FString& EventName, const TArray<FString>& SearchPaths);

	// Find blueprints with specific property values
	TSharedPtr<FJsonObject> FindPropertyToJson(const FString& PropertyName, const FString& PropertyValue, const FString& ParentClassName, const TArray<FString>& SearchPaths);

	// Search text across all blueprints (find in blueprints)
	TSharedPtr<FJsonObject> SearchInBlueprintsToJson(const FString& Query, const TArray<FString>& SearchPaths);

	// Export reference viewer graph (bidirectional)
	TSharedPtr<FJsonObject> ExportRefViewToJson(const FString& AssetPath, int32 DependencyDepth, int32 ReferencerDepth, bool bBlueprintsOnly);

	// Build full C++ reference audit across a search path (reverse-index of native symbols -> BP callers)
	TSharedPtr<FJsonObject> CppAuditToJson(const TArray<FString>& SearchPaths);

private:
	// CLI wrappers that call ToJson methods and output results
	void ExportBlueprint(const FString& BlueprintPath, bool bAnalyze);
	void ExportDirectory(const FString& DirectoryPath, bool bRecursive);
	void GetCppUsage(const FString& BlueprintPath);
	void GetReferences(const FString& BlueprintPath);
	void ExportGraph(const FString& RootPath, int32 MaxDepth);
	void FindBlueprintsCallingFunction(const FString& FunctionName, const FString& ClassName, const TArray<FString>& SearchPaths);
	void FindBlueprintsUsingVariable(const FString& VariableName, const FString& Kind, const TArray<FString>& SearchPaths);
	void FindNativeEventImplementations(const TArray<FString>& SearchPaths);
	void FindImplementableEventImplementations(const FString& EventName, const TArray<FString>& SearchPaths);
	void FindBlueprintsWithProperty(const FString& PropertyName, const FString& PropertyValue, const FString& ParentClassName, const TArray<FString>& SearchPaths);
	void ExportReferenceViewer(const FString& AssetPath, int32 DependencyDepth, int32 ReferencerDepth, bool bBlueprintsOnly);
	void BuildCppAudit(const TArray<FString>& SearchPaths);

	// Output JSON to stdout or file
	void OutputJson(const TSharedPtr<FJsonObject>& JsonObject);

	// Output plain text to stdout or file
	void OutputText(const FString& Text);

	// Output error in JSON format
	void OutputError(const FString& ErrorMessage);

	// Convert data structures to JSON (full mode)
	TSharedPtr<FJsonObject> BlueprintDataToJson(const struct FBlueprintExportData& Data, bool bIncludeFullGraph);
	TSharedPtr<FJsonObject> CppUsageToJson(const struct FBlueprintCppFunctionUsage& Usage);
	TSharedPtr<FJsonObject> ReferenceToJson(const struct FBlueprintReferenceData& Reference);
	TSharedPtr<FJsonObject> NodeToJson(const struct FBlueprintNodeData& Node);
	TSharedPtr<FJsonObject> PinToJson(const struct FBlueprintPinData& Pin);
	TSharedPtr<FJsonObject> ConnectionToJson(const struct FBlueprintConnectionData& Connection);

	// Compact pseudocode output
	FString BlueprintToCompact(const struct FBlueprintExportData& Data);
	FString FunctionToCompact(const struct FBlueprintFunctionData& Func);
	FString EventToCompact(const struct FBlueprintEventData& Event);
	FString NodesToCompact(const TArray<struct FBlueprintNodeData>& Nodes, const TArray<struct FBlueprintConnectionData>& Connections);

	// C++ skeleton output
	FString BlueprintToSkeleton(const struct FBlueprintExportData& Data);
	FString FunctionToSkeleton(const struct FBlueprintFunctionData& Func, const FString& ClassName);
	FString EventToSkeleton(const struct FBlueprintEventData& Event, const FString& ClassName);

	// Output file path (empty = stdout)
	FString OutputFilePath;

	// Current output mode
	EBlueprintExportMode OutputMode = EBlueprintExportMode::Compact;
};
