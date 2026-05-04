// BlueprintExportReader.h
// Recovered from UHT-generated files
// Original source for BlueprintAnalyzer plugin

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "BlueprintExportData.h"
#include "BlueprintExportReader.generated.h"

class UBlueprint;

/**
 * Core class for reading and exporting blueprint data
 */
UCLASS(BlueprintType)
class BLUEPRINTANALYZER_API UBlueprintExportReader : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Export a single blueprint to data structure
	 * @param BlueprintPath Path to the blueprint asset
	 * @return Exported blueprint data
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	FBlueprintExportData ExportBlueprint(const FString& BlueprintPath);

	/**
	 * Export a blueprint object directly
	 * @param Blueprint The blueprint to export
	 * @return Exported blueprint data
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	FBlueprintExportData ExportBlueprintObject(UBlueprint* Blueprint);

	/**
	 * Get all references from a blueprint
	 * @param BlueprintPath Path to the blueprint asset
	 * @param bIncludeSoftReferences Include soft object references
	 * @return Array of reference data
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	TArray<FBlueprintReferenceData> GetBlueprintReferences(const FString& BlueprintPath, bool bIncludeSoftReferences = true);

	/**
	 * Export multiple blueprints with reference context
	 * @param RootPath Root blueprint path to start from
	 * @param MaxDepth Maximum depth to traverse references
	 * @param bIncludeSoftReferences Include soft object references
	 * @return Map of blueprint paths to export data
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	TMap<FString, FBlueprintExportData> ExportBlueprintGraph(const FString& RootPath, int32 MaxDepth = 3, bool bIncludeSoftReferences = true);

	/**
	 * Find all blueprints that call a specific C++ function
	 * @param FunctionName Name of the function to search for
	 * @param FunctionClass Optional class name to filter by
	 * @param SearchPaths Paths to search in (empty = search all)
	 * @return Array of function usage data
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	TArray<FBlueprintCppFunctionUsage> FindBlueprintsCallingFunction(const FString& FunctionName, const FString& FunctionClass, UPARAM(ref) const TArray<FString>& SearchPaths);

	/**
	 * Find all blueprints reading from or writing to a named variable
	 * Scans K2Node_VariableGet and K2Node_VariableSet nodes across all graphs.
	 * @param VariableName Name of the variable to search for
	 * @param Kind "get" | "set" | "any" (default "any")
	 * @param SearchPaths Paths to search in (empty = search all)
	 * @return Array of variable usage sites
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	TArray<FBlueprintVariableUsage> FindBlueprintsUsingVariable(
		const FString& VariableName,
		const FString& Kind,
		UPARAM(ref) const TArray<FString>& SearchPaths);

	/**
	 * Find all blueprints implementing BlueprintNativeEvents
	 * @param SearchPaths Paths to search in (empty = search all)
	 * @return Array of function usage data for implemented events
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	TArray<FBlueprintCppFunctionUsage> FindBlueprintNativeEventImplementations(UPARAM(ref) const TArray<FString>& SearchPaths);

	/**
	 * Get detailed C++ function usage for a specific blueprint
	 * @param BlueprintPath Path to the blueprint asset
	 * @return Array of all C++ function calls in the blueprint
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	TArray<FBlueprintCppFunctionUsage> GetBlueprintCppFunctionUsage(const FString& BlueprintPath);

	/**
	 * Find blueprints implementing a specific BlueprintImplementableEvent
	 * @param EventName Name of the event to search for (e.g., "OnEmptyCowboyRecipeLoaded")
	 * @param SearchPaths Paths to search in
	 * @return Array of blueprints implementing the event
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	TArray<FBlueprintCppFunctionUsage> FindBlueprintImplementableEventImplementations(
		const FString& EventName,
		UPARAM(ref) const TArray<FString>& SearchPaths);

	/**
	 * Find blueprints with a specific CDO property value
	 * Searches for properties on the Class Default Object (CDO) - useful for finding
	 * Blueprint classes that have specific C++ base class property values set.
	 * @param PropertyName Name of the property to search for
	 * @param PropertyValue Optional value to filter by (empty = any value, "true"/"false" for bools)
	 * @param ParentClassName Optional parent class name to filter by (only search subclasses)
	 * @param SearchPaths Paths to search in
	 * @return Array of blueprints matching the criteria
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	TArray<FBlueprintPropertySearchResult> FindBlueprintsWithPropertyValue(
		const FString& PropertyName,
		const FString& PropertyValue,
		const FString& ParentClassName,
		UPARAM(ref) const TArray<FString>& SearchPaths);

	/**
	 * Search text across all blueprints in the given paths (like Find in Blueprints)
	 * Searches node titles, comments, pin names, pin defaults, variable names,
	 * function names, and event names.
	 * @param Query Text to search for (case-insensitive substring match)
	 * @param SearchPaths Paths to search in
	 * @return Array of search hits
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	TArray<FBlueprintSearchResult> SearchInBlueprints(
		const FString& Query,
		UPARAM(ref) const TArray<FString>& SearchPaths);

	/**
	 * Get assets that reference (depend on) this asset
	 * This is the reverse of GetBlueprintReferences - finds what USES this asset
	 * @param AssetPath Path to the asset
	 * @param bIncludeSoftReferences Include soft object references
	 * @return Array of referencer paths
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	TArray<FBlueprintReferenceData> GetAssetReferencers(const FString& AssetPath, bool bIncludeSoftReferences = true);

	/**
	 * Walk a Blueprint's UMG WidgetTree (no-op for non-UWidgetBlueprint assets),
	 * populating OutNodes flat with parent-before-child order and ParentIndex
	 * links. Property dump is class-aware: emits the curated style set
	 * (Visibility, ToolTipText, Text, Font, ColorAndOpacity, etc.) for any
	 * field the widget class exposes; non-applicable fields are silently
	 * skipped. Values are UE-text-format via ExportTextItem so they round-trip
	 * through the edit-side ImportText path.
	 *
	 * Shared between ExportBlueprint and the bulk widget-tree audit so the
	 * single-BP and many-BP paths emit identical per-widget data.
	 */
	static void WalkWidgetTreeFlat(class UBlueprint* Blueprint, TArray<FBlueprintWidgetTreeNode>& OutNodes);

	/**
	 * Build a reverse index of every native C++ symbol referenced by any Blueprint
	 * under SearchPaths. Walks parent class, K2Node_CallFunction, K2Node_VariableGet/Set,
	 * K2Node_BaseMCDelegate subclasses (Add/Remove/Create/Assign/Call),
	 * K2Node_DynamicCast, K2Node_MakeStruct/BreakStruct, K2Node_Event overrides of
	 * native signatures, and every Blueprint member variable whose type resolves to
	 * a native UClass or UScriptStruct.
	 *
	 * Returns both the reverse index (symbol -> callers) and the forward index
	 * (BP -> symbols). Use the reverse index to answer "what BPs break if I delete X?".
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	FBlueprintCppAudit BuildCppReferenceAudit(UPARAM(ref) const TArray<FString>& SearchPaths);

	/**
	 * Build a complete reference viewer graph (bidirectional)
	 * Like the Editor's Reference Viewer, shows both what an asset uses and what uses it
	 * @param RootAssetPath The asset to center the graph on
	 * @param DependencyDepth How deep to traverse dependencies (what it uses), 0 = none
	 * @param ReferencerDepth How deep to traverse referencers (what uses it), 0 = none
	 * @param bIncludeSoftReferences Include soft references
	 * @param bBlueprintsOnly Only include Blueprint assets (filter out textures, materials, etc)
	 * @return Complete bidirectional reference graph
	 */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Export")
	FAssetReferenceGraph BuildReferenceViewerGraph(
		const FString& RootAssetPath,
		int32 DependencyDepth = 3,
		int32 ReferencerDepth = 3,
		bool bIncludeSoftReferences = true,
		bool bBlueprintsOnly = false);
};
