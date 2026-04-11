// BlueprintEditHelpers.h
// Shared utilities for FBlueprintEditOps: blueprint loading, pin type parsing,
// graph/node/pin lookups, and response object builders.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class FJsonObject;

namespace FBlueprintEditHelpers
{
	/** Helper context that bundles a loaded blueprint with a resolved graph
	 *  (used by every op that works against a specific function/event graph). */
	struct FBPGraphContext
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
	};

	/** Load a blueprint for editing. Handles the `_C` suffix fallback mirror of
	 *  BlueprintExportReader::ExportBlueprint. Returns nullptr on failure and fills
	 *  OutError with a human-readable reason. */
	UBlueprint* LoadBlueprintForEdit(const FString& BlueprintPath, FString& OutError);

	/** Extract a required string param. Fills OutError with a standard
	 *  `{success:false, error:"Missing required param: X"}` response on failure
	 *  and returns false. */
	bool RequireString(const TSharedPtr<FJsonObject>& Params, const FString& Field, FString& OutValue, TSharedPtr<FJsonObject>& OutError);

	/** Extract `path` + `graph` params and resolve both the blueprint and graph.
	 *  Returns false and fills OutError on any failure. */
	bool ResolveBPAndGraph(const TSharedPtr<FJsonObject>& Params, FBPGraphContext& Out, TSharedPtr<FJsonObject>& OutError);

	/** Return the blueprint's path name, or an empty string if null. */
	FString BPPath(UBlueprint* Blueprint);

	/** Parse a type string (same format produced by the reader's PinTypeToString)
	 *  back into an FEdGraphPinType.
	 *
	 *  Supported forms:
	 *    - Primitives: bool, byte, int, int64, float, real, string, name, text
	 *    - Convenience struct aliases: vector, vector2d, rotator, transform, color, linearcolor
	 *    - Object refs: object<ClassName> (short name or full /Script/... path)
	 *    - Class refs: class<ClassName>
	 *    - Struct refs: struct<StructName>
	 *    - Containers: TArray<T>, TSet<T>, TMap<K,V>
	 *
	 *  Returns false and fills OutError on parse failure. */
	bool ParsePinType(const FString& TypeString, FEdGraphPinType& OutPinType, FString& OutError);

	/** Build a standard edit-success response:
	 *      { "success": true, "path": <BlueprintPath>, "warnings": [...] (if any) }
	 *  The returned object is extensible — callers can add op-specific fields before returning. */
	TSharedPtr<FJsonObject> MakeEditSuccess(const FString& BlueprintPath, const TArray<FString>& Warnings = TArray<FString>());

	/** Build a standard edit-error response:
	 *      { "success": false, "error": <Error> } */
	TSharedPtr<FJsonObject> MakeEditError(const FString& Error);

	// --- Graph / node / pin lookups ---
	// Declared here so all phases share one helper namespace. Implementations for
	// FindGraphByName / FindNodeByGuid / FindPinByName / NodeToEditResponse land in
	// Phase D alongside the node-editing operations.

	UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName, FString& OutError);
	UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FGuid& NodeGuid);
	UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction);
	TSharedPtr<FJsonObject> NodeToEditResponse(UEdGraphNode* Node);
}
