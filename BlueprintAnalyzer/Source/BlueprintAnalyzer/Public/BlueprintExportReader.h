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
};
