// BlueprintExportData.h
// Recovered from UHT-generated files
// Original source for BlueprintAnalyzer plugin

#pragma once

#include "CoreMinimal.h"
#include "BlueprintExportData.generated.h"

/**
 * Pin data for Blueprint nodes
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintPinData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString PinName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString PinType;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString DefaultValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsArray = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsReference = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsConst = false;

	// Comma-separated list of `{NodeGuid}.{PinName}` sources/targets, one per
	// entry in UE4's Pin->LinkedTo array. Usually a single entry, but a few
	// UE4 BP constructs legitimately produce multi-entry LinkedTo on a single
	// pin — callers should tolerate both shapes:
	//
	//   - K2Node_MacroInstance exec pins (`execute` / `then`): fan-in is legal
	//     because macros are inlined at compile time, so multiple upstream
	//     events may share one macro-instance exec pin. Treat as real.
	//
	//   - `self` / target pins on CallFunction/VariableGet/VariableSet nodes:
	//     normally single-source, but reroute node chains can surface as
	//     multi-entry in some export paths. Present tool does not attempt to
	//     collapse reroute passthroughs — each LinkedTo entry is emitted as-is.
	//
	// For data-pin migration to C++, prefer picking the non-reroute terminal
	// source (walk `K2Node_Knot` chains to their ultimate producer) rather
	// than blindly consuming the first entry.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString LinkedTo;
};

/**
 * Node data for Blueprint graphs
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintNodeData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString NodeGuid;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString NodeType;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString NodeTitle;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString NodeComment;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintPinData> InputPins;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintPinData> OutputPins;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	float PositionX = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	float PositionY = 0.0f;

	// C++ Function metadata (for CallFunction nodes)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString FunctionName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString FunctionClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsBlueprintCallable = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsBlueprintNativeEvent = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsBlueprintImplementableEvent = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsNativeFunction = false;
};

/**
 * Connection data between Blueprint nodes
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintConnectionData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString SourceNodeGuid;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString SourcePinName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString TargetNodeGuid;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString TargetPinName;
};

/**
 * Event data from Blueprint event graphs
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintEventData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString EventName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString EventType;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintNodeData> Nodes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintConnectionData> Connections;
};

/**
 * Variable data from Blueprints
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintVariableData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString VariableName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString VariableType;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString Category;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString DefaultValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsPublic = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsReadOnly = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsBlueprintVisible = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsReplicated = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString ReplicationCondition;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString ToolTip;

	/** True when the variable's type references a class/struct that no longer exists. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsTypeBroken = false;
};

/**
 * Function data from Blueprints
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintFunctionData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString FunctionName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString Category;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintPinData> Inputs;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintPinData> Outputs;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsPure = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsStatic = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsConst = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsOverride = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString AccessSpecifier;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintNodeData> Nodes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintConnectionData> Connections;
};

/**
 * Event dispatcher data from Blueprints
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintDispatcherData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString DispatcherName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintPinData> Parameters;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString Category;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString ToolTip;
};

/**
 * Reference data for asset dependencies
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintReferenceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString ReferencePath;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString ReferenceType;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsHardReference = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString Context;
};

/**
 * Component data from Blueprints
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintComponentData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString ComponentName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString ComponentClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString ParentComponent;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsRootComponent = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FVector RelativeLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FRotator RelativeRotation = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FVector RelativeScale3D = FVector::OneVector;
};

/**
 * Main export data structure containing all Blueprint information
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintExportData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString BlueprintName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString BlueprintPath;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString ParentClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString BlueprintType;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FString> ImplementedInterfaces;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintVariableData> Variables;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintFunctionData> Functions;

	// Locally-defined macros. Stored as FBlueprintFunctionData because macros
	// are structurally graphs-with-pins: the tunnel entry/exit nodes supply
	// Inputs/Outputs, and the body nodes/connections match the function shape.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintFunctionData> Macros;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintEventData> EventGraph;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintDispatcherData> EventDispatchers;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintReferenceData> References;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintComponentData> Components;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString BlueprintDescription;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString BlueprintCategory;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsDataOnly = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsInterface = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsAbstract = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsDeprecated = false;
};

/**
 * Result from searching for blueprints with specific property values
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintPropertySearchResult
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString BlueprintPath;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString BlueprintName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString ParentClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString PropertyName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString PropertyValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString PropertyType;
};

/**
 * Asset node in reference viewer graph
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FAssetReferenceNode
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString AssetPath;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString AssetName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString AssetClass;

	// Depth from the root asset (0 = root, negative = dependents, positive = dependencies)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	int32 Depth = 0;

	// Assets this one depends ON (what it uses)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FString> Dependencies;

	// Assets that depend ON this one (what uses it)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FString> Referencers;

	// Whether this is a hard or soft reference from parent
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsHardReference = true;

	// Whether this is a Blueprint asset
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsBlueprint = false;

	// Whether this is a C++ class (native)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsNativeClass = false;
};

/**
 * Complete reference viewer graph data
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FAssetReferenceGraph
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString RootAssetPath;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	int32 DependencyDepth = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	int32 ReferencerDepth = 0;

	// All nodes in the graph, keyed by asset path
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TMap<FString, FAssetReferenceNode> Nodes;

	// Summary counts
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	int32 TotalDependencies = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	int32 TotalReferencers = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	int32 BlueprintCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	int32 NativeClassCount = 0;
};

/**
 * Variable usage result (K2Node_VariableGet / K2Node_VariableSet sites)
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintVariableUsage
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString VariableName;

	// Class that owns the variable (empty if the BP-local variable's owner couldn't be resolved).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString VariableClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString BlueprintPath;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString NodeGuid;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString GraphName;

	// "get" or "set"
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString AccessKind;
};

/**
 * C++ function usage tracking data
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintCppFunctionUsage
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString FunctionName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString FunctionClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString BlueprintPath;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString NodeGuid;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString GraphName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsBlueprintCallable = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsBlueprintNativeEvent = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsBlueprintImplementableEvent = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	bool bIsImplementation = false;
};

/**
 * A single inline reference from a Blueprint to a C++ symbol.
 * Used inside FBlueprintCppAuditBp to describe what a BP touches.
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintCppSymbolRef
{
	GENERATED_BODY()

	/** Short name of the symbol. Empty Owner + Kind=UClass means Name is a class. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString Name;

	/** Owning native class name (without prefix). Empty when the symbol IS a class. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString Owner;

	/**
	 * What kind of native symbol this is. One of:
	 *   UClass, USTRUCT, UFUNCTION, UPROPERTY,
	 *   BlueprintAssignable, BlueprintNativeEvent, BlueprintImplementableEvent, ParentClass
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString Kind;
};

/**
 * Reverse-index entry for a single C++ symbol: which BPs reference it.
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintCppAuditSymbol
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString Name;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString Owner;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString Kind;

	/** BP paths (e.g. /Game/Foo/BP_Bar) that reference this symbol at least once. Sorted. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FString> Callers;
};

/**
 * Forward-index entry for a single BP: parent class + every native symbol it touches.
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintCppAuditBp
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString BlueprintPath;

	/** The native parent class the BP inherits from, if any. Empty if parent is itself a BP. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString ParentCppClass;

	/** Every distinct native symbol this BP references (order-preserving, deduped). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintCppSymbolRef> References;
};

/**
 * Full C++ reference audit across a search path. Populated by
 * UBlueprintExportReader::BuildCppReferenceAudit.
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintCppAudit
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FString> SearchPaths;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	int32 BlueprintCount = 0;

	/** Reverse index: symbol -> BPs that reference it. Sorted by (Kind, Owner, Name). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintCppAuditSymbol> Symbols;

	/** Forward index: BP -> symbols it touches. Sorted by BlueprintPath. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	TArray<FBlueprintCppAuditBp> Blueprints;
};

/**
 * Search hit from a Find-in-Blueprints style text search
 */
USTRUCT(BlueprintType)
struct BLUEPRINTANALYZER_API FBlueprintSearchResult
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString BlueprintPath;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString GraphName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString NodeGuid;

	/** What matched: NodeTitle, NodeComment, PinName, PinDefault, VariableName, FunctionName, EventName */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString MatchField;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString MatchValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlueprintExport")
	FString NodeClass;
};
