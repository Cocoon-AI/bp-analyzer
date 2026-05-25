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
class FProperty;

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

	/** Find an event-dispatcher signature graph by name (UBlueprint::DelegateSignatureGraphs).
	 *  Returns nullptr when no dispatcher of that name exists. */
	UEdGraph* FindDelegateSignatureGraph(UBlueprint* Blueprint, const FName& Name);

	/** Tear down an event dispatcher the way the editor's "Delete Event Dispatcher"
	 *  does: remove its multicast-delegate member variable AND its signature graph,
	 *  then revalidate Create Delegate nodes. Removing only one orphans the other and
	 *  trips KismetCompiler's "No delegate property found" warning. Safe when the
	 *  member variable is already gone (recovers pre-orphaned signature graphs). */
	void RemoveEventDispatcher(UBlueprint* Blueprint, UEdGraph* SignatureGraph);
	UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction);
	TSharedPtr<FJsonObject> NodeToEditResponse(UEdGraphNode* Node);

	/** Walk a dotted property path (e.g. "Font.Size", "MissionObject.TargetValue")
	 *  starting from a UObject. Two intermediate hop kinds are supported:
	 *
	 *    - FStructProperty: descends into the struct's inner FProperties without
	 *      changing the containing UObject (used by UMG widget set-property for
	 *      Font.Size, ColorAndOpacity.R, etc).
	 *    - FObjectProperty: dereferences the UObject* and uses the inner object
	 *      as the new container (used by cdo get to drill into subobject CDOs
	 *      like USDMissionAsset.MissionObject.TargetValue). FClassProperty
	 *      (which inherits from FObjectProperty) is rejected — UClass* targets
	 *      have no instance state to descend into.
	 *
	 *  On success: OutLeaf is the final FProperty, OutPtr is the raw memory
	 *  address of its value (suitable for ImportText/ExportTextItem), and
	 *  OutInnermost is the UObject* that directly owns OutLeaf (== Container
	 *  if no object hops, else the deepest dereferenced subobject; needed as
	 *  the Parent arg for ExportTextItem so relative object-ref resolution
	 *  works).
	 *
	 *  Returns false with a reason on any walk failure (missing prop, non-
	 *  struct/non-object intermediate, null subobject mid-chain). */
	bool ResolvePropertyPath(
		UObject* Container,
		const FString& DottedPath,
		FProperty*& OutLeaf,
		void*& OutPtr,
		UObject*& OutInnermost,
		FString& OutError);
}
