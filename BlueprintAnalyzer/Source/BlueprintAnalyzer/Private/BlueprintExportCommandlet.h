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

	// Bulk widget-tree audit across a search path. Loads every UWidgetBlueprint
	// under SearchPaths, walks its WidgetTree, applies the filters, and emits
	// either nested-per-BP (default) or flat one-row-per-widget (bFlat) JSON.
	//
	// ClassFilter: empty = no filter; otherwise widget classes (with or without
	//   the leading 'U') are matched against this set; non-matching widgets are
	//   dropped (in nested mode, ancestors are kept if they have surviving
	//   matched descendants — pruned otherwise).
	// PropFilter: empty = emit all keys from the curated whitelist; otherwise
	//   only emit listed keys in each widget's properties dict.
	// WhereFilters: array of (PropName, Substring); a widget passes only if
	//   for every entry, its properties[PropName] contains Substring (AND).
	TSharedPtr<FJsonObject> WidgetTreeAuditToJson(
		const TArray<FString>& SearchPaths,
		const TArray<FString>& ClassFilter,
		const TArray<FString>& PropFilter,
		const TArray<TPair<FString, FString>>& WhereFilters,
		bool bFlat);

	// Generate C++ UPROPERTY declarations from a BP's variables + dispatchers.
	// VarsFilter is an optional whitelist matched against RAW BP names (empty = all).
	// Category overrides the emitted Category specifier (empty = derive from BP).
	// bRawNames preserves the original BP var names verbatim in the emitted C++;
	// default (false) applies the same strip-spaces + PascalCase transform that
	// `edit variable lift` uses, so cppgen output is valid C++ identifiers and
	// matches the names lift will produce.
	TSharedPtr<FJsonObject> CppGenUPropertysToJson(const FString& BlueprintPath, const TArray<FString>& VarsFilter, const FString& Category, bool bRawNames);

	// --- DataTable inspection (BlueprintExportDataTable.cpp) ---
	// Read-only loaders for UDataTable assets. The RowStruct can be any USTRUCT
	// (game-defined or engine-defined); column values are exported via
	// FProperty::ExportTextItem to the same UE text format used elsewhere in
	// digbp (cdo get, component set-property, etc).

	// Row-struct schema: { row_struct, row_struct_path, columns: [{name, type, class}, ...] }
	TSharedPtr<FJsonObject> DataTableSchemaToJson(const FString& Path);

	// Just the row keys: { row_keys: ["bnd_pass01_01", ...], row_count }
	TSharedPtr<FJsonObject> DataTableRowsToJson(const FString& Path);

	// Single row by name: { row_key, row_struct, row_data: { col_name: text, ... } }
	TSharedPtr<FJsonObject> DataTableGetRowToJson(const FString& Path, const FString& RowKey);

	// Full table contents: { row_struct, row_count, rows: { row_key: { col: text }, ... } }
	TSharedPtr<FJsonObject> DataTableDumpToJson(const FString& Path);

	// --- DataTable write operations ---
	// All writes mutate the loaded UDataTable in memory and mark its package
	// dirty; callers must `datatable save` (or equivalent) to persist. SCC
	// (p4 edit / git) is the caller's responsibility.

	// Single-field write: ImportText the value into row[column]. Mirrors the
	// UE text-format that DataTableGetRowToJson returns.
	TSharedPtr<FJsonObject> DataTableSetCellToJson(const FString& Path, const FString& RowKey, const FString& Column, const FString& Value);

	// Batch write: Updates is a JSON object of shape
	//   { "row_key": { "column": "value", ... }, ... }
	// Failure on any (row, col) aborts the batch — no partial writes. The
	// failing row+col is reported in the error.
	TSharedPtr<FJsonObject> DataTableSetManyToJson(const FString& Path, const TSharedPtr<FJsonObject>& Updates);

	// Persist the table's package to disk (if dirty). Same semantics as
	// edit.save — bOnlyDirty=true.
	TSharedPtr<FJsonObject> DataTableSaveToJson(const FString& Path);

	// Bulk substring rewrite across every FSoftObjectProperty / FSoftClassProperty
	// in every row. Used for asset-move cleanup (e.g. /Game/Showdown/NFT/...
	// → /Game/Showdown/Characters/.../). Returns { rewritten_count, changes }.
	TSharedPtr<FJsonObject> DataTableRewritePathsToJson(const FString& Path, const FString& From, const FString& To);

	// --- UFont composite-font operations (BlueprintExportFont.cpp) ---
	// Inspect/mutate the protected UFont::CompositeFont UPROPERTY (reached via
	// reflection; editor Python refuses it). Built for l10n fallback wiring:
	// append culture/range-filtered sub-typefaces and set the last-resort
	// fallback typeface so CJK/Cyrillic renders in project FontFace assets
	// instead of engine DroidSansFallback. Mutations follow the editor commit
	// path (Modify/PreEditChange/PostEditChangeProperty) and mark the package
	// dirty; persist with FontSaveToJson (generic asset save — not a BP).

	// { font_cache_type, composite_font: { default_typeface, fallback, sub_typefaces } }
	TSharedPtr<FJsonObject> FontExportToJson(const FString& Path);

	// Append an FCompositeSubFont. Ranges is comma-separated unicode-hex
	// ("4E00-9FFF,3040-30FF"), required — a sub-font with no ranges never
	// matches. Cultures is optional comma/semicolon-separated filter.
	// BeforeIndex inserts ahead of an existing sub-font (first-match-wins
	// ordering); -1 appends.
	TSharedPtr<FJsonObject> FontAddSubfontToJson(const FString& Path, const FString& FontFacePath, const FString& Cultures, const FString& Ranges, const FString& TypefaceName, const FString& EditorName, double ScalingFactor, int32 BeforeIndex);

	// Replace FallbackTypeface with a single entry pointing at the FontFace.
	TSharedPtr<FJsonObject> FontSetFallbackToJson(const FString& Path, const FString& FontFacePath, const FString& TypefaceName, double ScalingFactor);

	// Remove a sub-typeface by index (indices as reported by FontExportToJson).
	TSharedPtr<FJsonObject> FontRemoveSubfontToJson(const FString& Path, int32 Index);

	// Persist the font's package to disk (if dirty). Same semantics as
	// datatable.save — bOnlyDirty=true.
	TSharedPtr<FJsonObject> FontSaveToJson(const FString& Path);

	// --- Animation asset export (BlueprintExportAnim.cpp) ---
	// Read-only JSON export dispatching on asset class: BlendSpace family
	// (axes, interpolation, samples), UAnimSequence (length/frames/additive/
	// sync markers/notifies/curves), UAnimMontage (blend, slots, sections).
	// bTracks (AnimSequence only) adds per-bone local-space transforms at
	// TracksFrame, read from the uncompressed RawAnimationData keys.
	TSharedPtr<FJsonObject> AnimExportToJson(const FString& Path, bool bTracks = false, int32 TracksFrame = 0);

	// --- Animation asset mutation (BlueprintExportAnimEdit.cpp) ---
	// Import saves immediately; everything else stages in memory and
	// persists via AnimSaveToJson (anim assets are not Blueprints — the
	// Blueprint edit save path does not apply). Blend space mutations end
	// with a full grid resample (vendored Persona triangulation) so the
	// asset blends at runtime without ever being opened in the editor.
	// bPreserveLocalTransform: import authored FBX locals directly instead of
	// reconstructing local-from-global (workaround for per-bone reconstruction
	// corruption on some rigs/poses).
	TSharedPtr<FJsonObject> AnimImportFbxToJson(const FString& FbxPath, const FString& SkeletonPath, const FString& DestPath, bool bPreserveLocalTransform = false);
	TSharedPtr<FJsonObject> AnimSetAdditiveToJson(const FString& Path, const FString& TypeStr, const FString& BasePosePath, const FString& BasePoseTypeStr, int32 RefFrame);
	TSharedPtr<FJsonObject> AnimBlendSpaceCreateToJson(const FString& DestPath, const FString& ClassName, const FString& SkeletonPath);
	TSharedPtr<FJsonObject> AnimBlendSpaceSetAxisToJson(const FString& Path, int32 AxisIndex, const FString& AxisName, bool bHasMin, double Min, bool bHasMax, double Max, bool bHasGridNum, int32 GridNum);
	TSharedPtr<FJsonObject> AnimBlendSpaceAddSampleToJson(const FString& Path, const FString& AnimationPath, double X, double Y);
	TSharedPtr<FJsonObject> AnimBlendSpaceRemoveSampleToJson(const FString& Path, int32 SampleIndex);
	TSharedPtr<FJsonObject> AnimSaveToJson(const FString& Path);

	// --- Level / World Composition export (BlueprintExportLevel.cpp) ---
	// Read-only. Loads the map package (no world init, no streaming).
	// LevelListToJson: asset-registry World assets under Dir (recursive).
	// LevelTilesToJson: UWorldComposition tile table (tiles read from
	// package summaries, never fully loaded) + persistent-level actors.
	// LevelActorsToJson: ULevel::Actors dump with components/bounds.
	// LevelLandscapeToJson: height/weight sampling via FLandscapeEditDataInterface.
	// bUnload: UnloadPackages + GC after export (batch walks).
	TSharedPtr<FJsonObject> LevelListToJson(const FString& Dir);
	TSharedPtr<FJsonObject> LevelTilesToJson(const FString& Path);
	TSharedPtr<FJsonObject> LevelActorsToJson(const FString& Path, const TArray<FString>& ClassFilters, bool bBoundsOnly, bool bInstances, bool bUnload);
	TSharedPtr<FJsonObject> LevelLandscapeToJson(const FString& Path, int32 GridN, const TArray<FVector2D>& Points, bool bLayers, bool bUnload);

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
