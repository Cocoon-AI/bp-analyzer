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
