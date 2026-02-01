// BlueprintExportCommandlet.cpp
// Commandlet implementation for CLI Blueprint analysis

#include "BlueprintExportCommandlet.h"
#include "BlueprintExportReader.h"
#include "BlueprintExportData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"

// Markers for JSON output parsing
#define JSON_START_MARKER TEXT("__JSON_START__")
#define JSON_END_MARKER TEXT("__JSON_END__")

//------------------------------------------------------------------------------
// Static Helper Functions
//------------------------------------------------------------------------------

// Helper to check if a node should be skipped in output (reroute, knot nodes)
static bool ShouldSkipNode(const FBlueprintNodeData& Node)
{
	// Check node type
	if (Node.NodeType.Contains(TEXT("Reroute")))
	{
		return true;
	}
	if (Node.NodeType.Contains(TEXT("Knot")))
	{
		return true;
	}
	// Also check node title (some nodes have generic type but specific title)
	if (Node.NodeTitle.Contains(TEXT("Reroute")))
	{
		return true;
	}
	return false;
}

// Helper to clean event name for C++ function name
static FString CleanEventName(const FString& EventName)
{
	FString Result = EventName;

	// Remove common prefixes
	Result.RemoveFromStart(TEXT("Event "));
	Result.RemoveFromStart(TEXT("Event"));

	// Remove newlines and extra text (like "Custom Event" suffix)
	int32 NewlineIdx;
	if (Result.FindChar('\n', NewlineIdx))
	{
		Result = Result.Left(NewlineIdx);
	}

	// Remove spaces for function name
	Result.ReplaceInline(TEXT(" "), TEXT(""));

	return Result;
}

// Helper to convert Blueprint pin type to C++ type
static FString BPTypeToCppType(const FString& BPType)
{
	FString Result = BPType;

	// Handle object references: object<ClassName> -> UClassName*
	if (Result.StartsWith(TEXT("object<")) && Result.EndsWith(TEXT(">")))
	{
		FString Inner = Result.Mid(7, Result.Len() - 8);
		Result = TEXT("U") + Inner + TEXT("*");
	}
	// Handle class references: class<ClassName> -> TSubclassOf<UClassName>
	else if (Result.StartsWith(TEXT("class<")) && Result.EndsWith(TEXT(">")))
	{
		FString Inner = Result.Mid(6, Result.Len() - 7);
		Result = TEXT("TSubclassOf<U") + Inner + TEXT(">");
	}
	// Handle basic types
	else if (Result == TEXT("bool"))
	{
		Result = TEXT("bool");
	}
	else if (Result == TEXT("int") || Result == TEXT("integer"))
	{
		Result = TEXT("int32");
	}
	else if (Result == TEXT("float") || Result == TEXT("real"))
	{
		Result = TEXT("float");
	}
	else if (Result == TEXT("string"))
	{
		Result = TEXT("FString");
	}
	else if (Result == TEXT("name"))
	{
		Result = TEXT("FName");
	}
	else if (Result == TEXT("text"))
	{
		Result = TEXT("FText");
	}
	else if (Result == TEXT("vector"))
	{
		Result = TEXT("FVector");
	}
	else if (Result == TEXT("rotator"))
	{
		Result = TEXT("FRotator");
	}
	else if (Result == TEXT("transform"))
	{
		Result = TEXT("FTransform");
	}

	return Result;
}

//------------------------------------------------------------------------------

UBlueprintExportCommandlet::UBlueprintExportCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;  // Need editor functionality for Blueprint access
	LogToConsole = false;  // We control output via JSON markers
	ShowErrorCount = false;
}

int32 UBlueprintExportCommandlet::Main(const FString& Params)
{
	// Parse command line
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> ParamsMap;
	ParseCommandLine(*Params, Tokens, Switches, ParamsMap);

	// Get parameters
	FString BlueprintPath;
	FString DirectoryPath;
	FString FunctionName;
	FString ClassName;
	FString FindPropertyName;
	FString PropertyValue;
	FString ParentClassName;
	FString EventName;
	bool bAnalyze = Switches.Contains(TEXT("analyze"));
	bool bRecursive = !Switches.Contains(TEXT("norecurse"));
	bool bCppUsage = Switches.Contains(TEXT("cppusage"));
	bool bReferences = Switches.Contains(TEXT("references"));
	bool bGraph = Switches.Contains(TEXT("graph"));
	bool bNativeEvents = Switches.Contains(TEXT("nativeevents"));
	bool bRefView = Switches.Contains(TEXT("refview"));
	bool bBlueprintsOnly = Switches.Contains(TEXT("bponly"));
	int32 MaxDepth = 3;
	int32 RefDepth = 3;
	int32 ReferDepth = 3;

	// Output mode (default is Compact)
	OutputMode = EBlueprintExportMode::Compact;
	if (Switches.Contains(TEXT("json")))
	{
		OutputMode = EBlueprintExportMode::Json;
	}
	else if (Switches.Contains(TEXT("skeleton")))
	{
		OutputMode = EBlueprintExportMode::Skeleton;
	}

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
	if (ParamsMap.Contains(TEXT("out")))
	{
		OutputFilePath = ParamsMap[TEXT("out")];
	}
	if (ParamsMap.Contains(TEXT("depth")))
	{
		MaxDepth = FCString::Atoi(*ParamsMap[TEXT("depth")]);
	}
	if (ParamsMap.Contains(TEXT("refdepth")))
	{
		RefDepth = FCString::Atoi(*ParamsMap[TEXT("refdepth")]);
	}
	if (ParamsMap.Contains(TEXT("referdepth")))
	{
		ReferDepth = FCString::Atoi(*ParamsMap[TEXT("referdepth")]);
	}
	if (ParamsMap.Contains(TEXT("findprop")))
	{
		FindPropertyName = ParamsMap[TEXT("findprop")];
	}
	if (ParamsMap.Contains(TEXT("propvalue")))
	{
		PropertyValue = ParamsMap[TEXT("propvalue")];
	}
	if (ParamsMap.Contains(TEXT("parentclass")))
	{
		ParentClassName = ParamsMap[TEXT("parentclass")];
	}
	if (ParamsMap.Contains(TEXT("event")))
	{
		EventName = ParamsMap[TEXT("event")];
	}

	// Ensure asset registry is ready
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Wait for asset registry to finish loading
	if (AssetRegistry.IsLoadingAssets())
	{
		AssetRegistry.SearchAllAssets(true);
	}

	// Execute appropriate operation based on parameters
	if (!BlueprintPath.IsEmpty())
	{
		if (bCppUsage)
		{
			GetCppUsage(BlueprintPath);
		}
		else if (bReferences)
		{
			GetReferences(BlueprintPath);
		}
		else if (bGraph)
		{
			ExportGraph(BlueprintPath, MaxDepth);
		}
		else if (bRefView)
		{
			ExportReferenceViewer(BlueprintPath, RefDepth, ReferDepth, bBlueprintsOnly);
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
		else if (!FindPropertyName.IsEmpty())
		{
			TArray<FString> SearchPaths;
			SearchPaths.Add(DirectoryPath);
			FindBlueprintsWithProperty(FindPropertyName, PropertyValue, ParentClassName, SearchPaths);
		}
		else if (!EventName.IsEmpty())
		{
			TArray<FString> SearchPaths;
			SearchPaths.Add(DirectoryPath);
			FindImplementableEventImplementations(EventName, SearchPaths);
		}
		else if (bNativeEvents)
		{
			TArray<FString> SearchPaths;
			SearchPaths.Add(DirectoryPath);
			FindNativeEventImplementations(SearchPaths);
		}
		else
		{
			ExportDirectory(DirectoryPath, bRecursive);
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

	// Output based on mode
	switch (OutputMode)
	{
	case EBlueprintExportMode::Compact:
		OutputText(BlueprintToCompact(ExportData));
		break;

	case EBlueprintExportMode::Skeleton:
		OutputText(BlueprintToSkeleton(ExportData));
		break;

	case EBlueprintExportMode::Json:
	default:
		{
			TSharedPtr<FJsonObject> JsonObject = BlueprintDataToJson(ExportData, true);
			JsonObject->SetBoolField(TEXT("success"), true);

			if (bAnalyze)
			{
				// Add complexity analysis
				TSharedPtr<FJsonObject> Analysis = MakeShareable(new FJsonObject);

				int32 TotalNodes = 0;
				int32 TotalConnections = 0;

				for (const FBlueprintFunctionData& Func : ExportData.Functions)
				{
					TotalNodes += Func.Nodes.Num();
					TotalConnections += Func.Connections.Num();
				}

				for (const FBlueprintEventData& Event : ExportData.EventGraph)
				{
					TotalNodes += Event.Nodes.Num();
					TotalConnections += Event.Connections.Num();
				}

				Analysis->SetNumberField(TEXT("total_functions"), ExportData.Functions.Num());
				Analysis->SetNumberField(TEXT("total_variables"), ExportData.Variables.Num());
				Analysis->SetNumberField(TEXT("total_events"), ExportData.EventGraph.Num());
				Analysis->SetNumberField(TEXT("total_nodes"), TotalNodes);
				Analysis->SetNumberField(TEXT("total_connections"), TotalConnections);
				Analysis->SetNumberField(TEXT("total_components"), ExportData.Components.Num());
				Analysis->SetNumberField(TEXT("total_references"), ExportData.References.Num());

				// Complexity score (simple heuristic)
				float Complexity = TotalNodes * 1.0f + TotalConnections * 0.5f +
					ExportData.Functions.Num() * 2.0f + ExportData.Variables.Num() * 0.5f;
				Analysis->SetNumberField(TEXT("complexity_score"), Complexity);

				JsonObject->SetObjectField(TEXT("analysis"), Analysis);
			}

			OutputJson(JsonObject);
		}
		break;
	}
}

void UBlueprintExportCommandlet::ExportDirectory(const FString& DirectoryPath, bool bRecursive)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssetsByPath(FName(*DirectoryPath), AssetList, bRecursive);

	TArray<TSharedPtr<FJsonValue>> BlueprintArray;

	for (const FAssetData& Asset : AssetList)
	{
		if (Asset.AssetClass == UBlueprint::StaticClass()->GetFName() ||
			Asset.AssetClass.ToString().Contains(TEXT("Blueprint")))
		{
			TSharedPtr<FJsonObject> BpInfo = MakeShareable(new FJsonObject);
			BpInfo->SetStringField(TEXT("path"), Asset.ObjectPath.ToString());
			BpInfo->SetStringField(TEXT("name"), Asset.AssetName.ToString());
			BpInfo->SetStringField(TEXT("class"), Asset.AssetClass.ToString());
			BlueprintArray.Add(MakeShareable(new FJsonValueObject(BpInfo)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("directory"), DirectoryPath);
	Result->SetNumberField(TEXT("count"), BlueprintArray.Num());
	Result->SetArrayField(TEXT("blueprints"), BlueprintArray);

	OutputJson(Result);
}

void UBlueprintExportCommandlet::GetCppUsage(const FString& BlueprintPath)
{
	UBlueprintExportReader* Reader = NewObject<UBlueprintExportReader>();
	TArray<FBlueprintCppFunctionUsage> Usage = Reader->GetBlueprintCppFunctionUsage(BlueprintPath);

	TArray<TSharedPtr<FJsonValue>> UsageArray;
	for (const FBlueprintCppFunctionUsage& Item : Usage)
	{
		UsageArray.Add(MakeShareable(new FJsonValueObject(CppUsageToJson(Item))));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_path"), BlueprintPath);
	Result->SetNumberField(TEXT("count"), UsageArray.Num());
	Result->SetArrayField(TEXT("cpp_usage"), UsageArray);

	OutputJson(Result);
}

void UBlueprintExportCommandlet::GetReferences(const FString& BlueprintPath)
{
	UBlueprintExportReader* Reader = NewObject<UBlueprintExportReader>();
	TArray<FBlueprintReferenceData> References = Reader->GetBlueprintReferences(BlueprintPath, true);

	TArray<TSharedPtr<FJsonValue>> RefArray;
	for (const FBlueprintReferenceData& Ref : References)
	{
		RefArray.Add(MakeShareable(new FJsonValueObject(ReferenceToJson(Ref))));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_path"), BlueprintPath);
	Result->SetNumberField(TEXT("count"), RefArray.Num());
	Result->SetArrayField(TEXT("references"), RefArray);

	OutputJson(Result);
}

void UBlueprintExportCommandlet::ExportGraph(const FString& RootPath, int32 MaxDepth)
{
	UBlueprintExportReader* Reader = NewObject<UBlueprintExportReader>();
	TMap<FString, FBlueprintExportData> Graph = Reader->ExportBlueprintGraph(RootPath, MaxDepth, true);

	TSharedPtr<FJsonObject> GraphObj = MakeShareable(new FJsonObject);
	for (auto& Pair : Graph)
	{
		GraphObj->SetObjectField(Pair.Key, BlueprintDataToJson(Pair.Value, OutputMode == EBlueprintExportMode::Json));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("root_path"), RootPath);
	Result->SetNumberField(TEXT("max_depth"), MaxDepth);
	Result->SetNumberField(TEXT("blueprint_count"), Graph.Num());
	Result->SetObjectField(TEXT("graph"), GraphObj);

	OutputJson(Result);
}

void UBlueprintExportCommandlet::FindBlueprintsCallingFunction(const FString& FunctionName, const FString& ClassName, const TArray<FString>& SearchPaths)
{
	UBlueprintExportReader* Reader = NewObject<UBlueprintExportReader>();
	TArray<FBlueprintCppFunctionUsage> Results = Reader->FindBlueprintsCallingFunction(FunctionName, ClassName, SearchPaths);

	TArray<TSharedPtr<FJsonValue>> ResultArray;
	for (const FBlueprintCppFunctionUsage& Item : Results)
	{
		ResultArray.Add(MakeShareable(new FJsonValueObject(CppUsageToJson(Item))));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("function_name"), FunctionName);
	Result->SetStringField(TEXT("function_class"), ClassName);
	Result->SetNumberField(TEXT("count"), ResultArray.Num());
	Result->SetArrayField(TEXT("callers"), ResultArray);

	OutputJson(Result);
}

void UBlueprintExportCommandlet::FindNativeEventImplementations(const TArray<FString>& SearchPaths)
{
	UBlueprintExportReader* Reader = NewObject<UBlueprintExportReader>();
	TArray<FBlueprintCppFunctionUsage> Results = Reader->FindBlueprintNativeEventImplementations(SearchPaths);

	TArray<TSharedPtr<FJsonValue>> ResultArray;
	for (const FBlueprintCppFunctionUsage& Item : Results)
	{
		ResultArray.Add(MakeShareable(new FJsonValueObject(CppUsageToJson(Item))));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("count"), ResultArray.Num());
	Result->SetArrayField(TEXT("implementations"), ResultArray);

	OutputJson(Result);
}

void UBlueprintExportCommandlet::FindBlueprintsWithProperty(const FString& PropertyName, const FString& PropertyValue, const FString& ParentClassName, const TArray<FString>& SearchPaths)
{
	UBlueprintExportReader* Reader = NewObject<UBlueprintExportReader>();
	TArray<FBlueprintPropertySearchResult> Results = Reader->FindBlueprintsWithPropertyValue(PropertyName, PropertyValue, ParentClassName, SearchPaths);

	TArray<TSharedPtr<FJsonValue>> ResultArray;
	for (const FBlueprintPropertySearchResult& Item : Results)
	{
		TSharedPtr<FJsonObject> ItemJson = MakeShareable(new FJsonObject);
		ItemJson->SetStringField(TEXT("blueprint_path"), Item.BlueprintPath);
		ItemJson->SetStringField(TEXT("blueprint_name"), Item.BlueprintName);
		ItemJson->SetStringField(TEXT("parent_class"), Item.ParentClass);
		ItemJson->SetStringField(TEXT("property_name"), Item.PropertyName);
		ItemJson->SetStringField(TEXT("property_value"), Item.PropertyValue);
		ItemJson->SetStringField(TEXT("property_type"), Item.PropertyType);
		ResultArray.Add(MakeShareable(new FJsonValueObject(ItemJson)));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("property_name"), PropertyName);
	if (!PropertyValue.IsEmpty())
	{
		Result->SetStringField(TEXT("property_value_filter"), PropertyValue);
	}
	if (!ParentClassName.IsEmpty())
	{
		Result->SetStringField(TEXT("parent_class_filter"), ParentClassName);
	}
	Result->SetNumberField(TEXT("count"), ResultArray.Num());
	Result->SetArrayField(TEXT("results"), ResultArray);

	OutputJson(Result);
}

void UBlueprintExportCommandlet::FindImplementableEventImplementations(const FString& EventName, const TArray<FString>& SearchPaths)
{
	UBlueprintExportReader* Reader = NewObject<UBlueprintExportReader>();
	TArray<FBlueprintCppFunctionUsage> Results = Reader->FindBlueprintImplementableEventImplementations(EventName, SearchPaths);

	TArray<TSharedPtr<FJsonValue>> ResultArray;
	for (const FBlueprintCppFunctionUsage& Item : Results)
	{
		ResultArray.Add(MakeShareable(new FJsonValueObject(CppUsageToJson(Item))));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("event_name"), EventName);
	Result->SetNumberField(TEXT("count"), ResultArray.Num());
	Result->SetArrayField(TEXT("implementations"), ResultArray);

	OutputJson(Result);
}

void UBlueprintExportCommandlet::ExportReferenceViewer(const FString& AssetPath, int32 DependencyDepth, int32 ReferencerDepth, bool bBlueprintsOnly)
{
	UBlueprintExportReader* Reader = NewObject<UBlueprintExportReader>();
	FAssetReferenceGraph Graph = Reader->BuildReferenceViewerGraph(AssetPath, DependencyDepth, ReferencerDepth, true, bBlueprintsOnly);

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("root_asset"), Graph.RootAssetPath);
	Result->SetNumberField(TEXT("dependency_depth"), Graph.DependencyDepth);
	Result->SetNumberField(TEXT("referencer_depth"), Graph.ReferencerDepth);
	Result->SetNumberField(TEXT("total_dependencies"), Graph.TotalDependencies);
	Result->SetNumberField(TEXT("total_referencers"), Graph.TotalReferencers);
	Result->SetNumberField(TEXT("blueprint_count"), Graph.BlueprintCount);
	Result->SetNumberField(TEXT("native_class_count"), Graph.NativeClassCount);
	Result->SetNumberField(TEXT("total_nodes"), Graph.Nodes.Num());

	// Convert nodes to JSON
	TSharedPtr<FJsonObject> NodesObj = MakeShareable(new FJsonObject);
	for (const auto& Pair : Graph.Nodes)
	{
		const FAssetReferenceNode& Node = Pair.Value;

		TSharedPtr<FJsonObject> NodeObj = MakeShareable(new FJsonObject);
		NodeObj->SetStringField(TEXT("path"), Node.AssetPath);
		NodeObj->SetStringField(TEXT("name"), Node.AssetName);
		NodeObj->SetStringField(TEXT("class"), Node.AssetClass);
		NodeObj->SetNumberField(TEXT("depth"), Node.Depth);
		NodeObj->SetBoolField(TEXT("is_blueprint"), Node.bIsBlueprint);
		NodeObj->SetBoolField(TEXT("is_native"), Node.bIsNativeClass);
		NodeObj->SetBoolField(TEXT("is_hard_ref"), Node.bIsHardReference);

		// Dependencies
		TArray<TSharedPtr<FJsonValue>> DepsArray;
		for (const FString& Dep : Node.Dependencies)
		{
			DepsArray.Add(MakeShareable(new FJsonValueString(Dep)));
		}
		NodeObj->SetArrayField(TEXT("dependencies"), DepsArray);

		// Referencers
		TArray<TSharedPtr<FJsonValue>> RefsArray;
		for (const FString& Ref : Node.Referencers)
		{
			RefsArray.Add(MakeShareable(new FJsonValueString(Ref)));
		}
		NodeObj->SetArrayField(TEXT("referencers"), RefsArray);

		NodesObj->SetObjectField(Pair.Key, NodeObj);
	}
	Result->SetObjectField(TEXT("nodes"), NodesObj);

	// Also create a compact summary for easier reading
	if (OutputMode == EBlueprintExportMode::Compact)
	{
		FString CompactOutput;
		CompactOutput += TEXT("# Reference Viewer: ") + Graph.RootAssetPath + TEXT("\n\n");

		// Separate nodes by depth for cleaner output
		TMap<int32, TArray<const FAssetReferenceNode*>> NodesByDepth;
		for (const auto& Pair : Graph.Nodes)
		{
			NodesByDepth.FindOrAdd(Pair.Value.Depth).Add(&Pair.Value);
		}

		// Sort depths
		TArray<int32> Depths;
		NodesByDepth.GetKeys(Depths);
		Depths.Sort();

		// Output referencers first (negative depths, but show positive for clarity)
		CompactOutput += TEXT("## What Uses This Asset (Referencers)\n");
		for (int32 Depth : Depths)
		{
			if (Depth < 0)
			{
				CompactOutput += FString::Printf(TEXT("\n### Depth %d:\n"), -Depth);
				for (const FAssetReferenceNode* Node : NodesByDepth[Depth])
				{
					FString TypeTag = Node->bIsBlueprint ? TEXT("[BP]") : (Node->bIsNativeClass ? TEXT("[C++]") : TEXT(""));
					CompactOutput += FString::Printf(TEXT("  %s %s (%s)\n"), *TypeTag, *Node->AssetName, *Node->AssetClass);
				}
			}
		}

		// Root
		CompactOutput += TEXT("\n## Root Asset\n");
		if (const FAssetReferenceNode* Root = Graph.Nodes.Find(Graph.RootAssetPath))
		{
			FString TypeTag = Root->bIsBlueprint ? TEXT("[BP]") : (Root->bIsNativeClass ? TEXT("[C++]") : TEXT(""));
			CompactOutput += FString::Printf(TEXT("  %s %s (%s)\n"), *TypeTag, *Root->AssetName, *Root->AssetClass);
		}

		// Dependencies (positive depths)
		CompactOutput += TEXT("\n## What This Asset Uses (Dependencies)\n");
		for (int32 Depth : Depths)
		{
			if (Depth > 0)
			{
				CompactOutput += FString::Printf(TEXT("\n### Depth %d:\n"), Depth);
				for (const FAssetReferenceNode* Node : NodesByDepth[Depth])
				{
					FString TypeTag = Node->bIsBlueprint ? TEXT("[BP]") : (Node->bIsNativeClass ? TEXT("[C++]") : TEXT(""));
					CompactOutput += FString::Printf(TEXT("  %s %s (%s)\n"), *TypeTag, *Node->AssetName, *Node->AssetClass);
				}
			}
		}

		CompactOutput += TEXT("\n## Summary\n");
		CompactOutput += FString::Printf(TEXT("  Total Nodes: %d\n"), Graph.Nodes.Num());
		CompactOutput += FString::Printf(TEXT("  Blueprints: %d\n"), Graph.BlueprintCount);
		CompactOutput += FString::Printf(TEXT("  Native Classes: %d\n"), Graph.NativeClassCount);
		CompactOutput += FString::Printf(TEXT("  Dependencies Found: %d\n"), Graph.TotalDependencies);
		CompactOutput += FString::Printf(TEXT("  Referencers Found: %d\n"), Graph.TotalReferencers);

		OutputText(CompactOutput);
		return;
	}

	OutputJson(Result);
}

void UBlueprintExportCommandlet::OutputJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

	if (OutputFilePath.IsEmpty())
	{
		// Output to stdout with markers for parsing
		UE_LOG(LogTemp, Display, TEXT("%s%s%s"), JSON_START_MARKER, *OutputString, JSON_END_MARKER);
	}
	else
	{
		// Write to file
		if (FFileHelper::SaveStringToFile(OutputString, *OutputFilePath))
		{
			UE_LOG(LogTemp, Display, TEXT("Output written to: %s"), *OutputFilePath);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to write output to: %s"), *OutputFilePath);
		}
	}
}

void UBlueprintExportCommandlet::OutputError(const FString& ErrorMessage)
{
	TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
	JsonObject->SetBoolField(TEXT("success"), false);
	JsonObject->SetStringField(TEXT("error"), ErrorMessage);
	OutputJson(JsonObject);
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::PinToJson(const FBlueprintPinData& Pin)
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject);
	Json->SetStringField(TEXT("name"), Pin.PinName);
	Json->SetStringField(TEXT("type"), Pin.PinType);
	if (!Pin.DefaultValue.IsEmpty())
	{
		Json->SetStringField(TEXT("default"), Pin.DefaultValue);
	}
	if (!Pin.LinkedTo.IsEmpty())
	{
		Json->SetStringField(TEXT("linked_to"), Pin.LinkedTo);
	}
	return Json;
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::NodeToJson(const FBlueprintNodeData& Node)
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject);
	Json->SetStringField(TEXT("guid"), Node.NodeGuid);
	Json->SetStringField(TEXT("type"), Node.NodeType);
	Json->SetStringField(TEXT("title"), Node.NodeTitle);
	if (!Node.NodeComment.IsEmpty())
	{
		Json->SetStringField(TEXT("comment"), Node.NodeComment);
	}
	if (!Node.FunctionName.IsEmpty())
	{
		Json->SetStringField(TEXT("function"), Node.FunctionName);
		Json->SetStringField(TEXT("function_class"), Node.FunctionClass);
		Json->SetBoolField(TEXT("is_native"), Node.bIsNativeFunction);
	}

	// Input pins
	TArray<TSharedPtr<FJsonValue>> InputPins;
	for (const FBlueprintPinData& Pin : Node.InputPins)
	{
		InputPins.Add(MakeShareable(new FJsonValueObject(PinToJson(Pin))));
	}
	if (InputPins.Num() > 0)
	{
		Json->SetArrayField(TEXT("inputs"), InputPins);
	}

	// Output pins
	TArray<TSharedPtr<FJsonValue>> OutputPins;
	for (const FBlueprintPinData& Pin : Node.OutputPins)
	{
		OutputPins.Add(MakeShareable(new FJsonValueObject(PinToJson(Pin))));
	}
	if (OutputPins.Num() > 0)
	{
		Json->SetArrayField(TEXT("outputs"), OutputPins);
	}

	return Json;
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::ConnectionToJson(const FBlueprintConnectionData& Connection)
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject);
	Json->SetStringField(TEXT("from"), FString::Printf(TEXT("%s.%s"), *Connection.SourceNodeGuid, *Connection.SourcePinName));
	Json->SetStringField(TEXT("to"), FString::Printf(TEXT("%s.%s"), *Connection.TargetNodeGuid, *Connection.TargetPinName));
	return Json;
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::BlueprintDataToJson(const FBlueprintExportData& Data, bool bIncludeFullGraph)
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject);

	Json->SetStringField(TEXT("blueprint_name"), Data.BlueprintName);
	Json->SetStringField(TEXT("blueprint_path"), Data.BlueprintPath);
	Json->SetStringField(TEXT("parent_class"), Data.ParentClass);
	Json->SetStringField(TEXT("blueprint_type"), Data.BlueprintType);
	Json->SetStringField(TEXT("description"), Data.BlueprintDescription);
	Json->SetStringField(TEXT("category"), Data.BlueprintCategory);
	Json->SetBoolField(TEXT("is_data_only"), Data.bIsDataOnly);
	Json->SetBoolField(TEXT("is_interface"), Data.bIsInterface);
	Json->SetBoolField(TEXT("is_abstract"), Data.bIsAbstract);
	Json->SetBoolField(TEXT("is_deprecated"), Data.bIsDeprecated);

	// Interfaces
	TArray<TSharedPtr<FJsonValue>> Interfaces;
	for (const FString& Interface : Data.ImplementedInterfaces)
	{
		Interfaces.Add(MakeShareable(new FJsonValueString(Interface)));
	}
	Json->SetArrayField(TEXT("implemented_interfaces"), Interfaces);

	// Variables
	TArray<TSharedPtr<FJsonValue>> Variables;
	for (const FBlueprintVariableData& Var : Data.Variables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShareable(new FJsonObject);
		VarObj->SetStringField(TEXT("name"), Var.VariableName);
		VarObj->SetStringField(TEXT("type"), Var.VariableType);
		VarObj->SetStringField(TEXT("category"), Var.Category);
		VarObj->SetStringField(TEXT("default_value"), Var.DefaultValue);
		VarObj->SetBoolField(TEXT("is_public"), Var.bIsPublic);
		VarObj->SetBoolField(TEXT("is_replicated"), Var.bIsReplicated);
		Variables.Add(MakeShareable(new FJsonValueObject(VarObj)));
	}
	Json->SetArrayField(TEXT("variables"), Variables);

	if (bIncludeFullGraph)
	{
		// Full function data with nodes and connections
		TArray<TSharedPtr<FJsonValue>> Functions;
		for (const FBlueprintFunctionData& Func : Data.Functions)
		{
			TSharedPtr<FJsonObject> FuncObj = MakeShareable(new FJsonObject);
			FuncObj->SetStringField(TEXT("name"), Func.FunctionName);
			FuncObj->SetStringField(TEXT("category"), Func.Category);
			FuncObj->SetBoolField(TEXT("is_pure"), Func.bIsPure);
			FuncObj->SetBoolField(TEXT("is_static"), Func.bIsStatic);
			FuncObj->SetBoolField(TEXT("is_const"), Func.bIsConst);
			FuncObj->SetBoolField(TEXT("is_override"), Func.bIsOverride);

			// Inputs
			TArray<TSharedPtr<FJsonValue>> Inputs;
			for (const FBlueprintPinData& Pin : Func.Inputs)
			{
				Inputs.Add(MakeShareable(new FJsonValueObject(PinToJson(Pin))));
			}
			FuncObj->SetArrayField(TEXT("inputs"), Inputs);

			// Outputs
			TArray<TSharedPtr<FJsonValue>> Outputs;
			for (const FBlueprintPinData& Pin : Func.Outputs)
			{
				Outputs.Add(MakeShareable(new FJsonValueObject(PinToJson(Pin))));
			}
			FuncObj->SetArrayField(TEXT("outputs"), Outputs);

			// Nodes
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (const FBlueprintNodeData& Node : Func.Nodes)
			{
				Nodes.Add(MakeShareable(new FJsonValueObject(NodeToJson(Node))));
			}
			FuncObj->SetArrayField(TEXT("nodes"), Nodes);

			// Connections
			TArray<TSharedPtr<FJsonValue>> Connections;
			for (const FBlueprintConnectionData& Conn : Func.Connections)
			{
				Connections.Add(MakeShareable(new FJsonValueObject(ConnectionToJson(Conn))));
			}
			FuncObj->SetArrayField(TEXT("connections"), Connections);

			Functions.Add(MakeShareable(new FJsonValueObject(FuncObj)));
		}
		Json->SetArrayField(TEXT("functions"), Functions);

		// Full event data with nodes and connections
		TArray<TSharedPtr<FJsonValue>> Events;
		for (const FBlueprintEventData& Event : Data.EventGraph)
		{
			TSharedPtr<FJsonObject> EventObj = MakeShareable(new FJsonObject);
			EventObj->SetStringField(TEXT("name"), Event.EventName);
			EventObj->SetStringField(TEXT("type"), Event.EventType);

			// Nodes
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (const FBlueprintNodeData& Node : Event.Nodes)
			{
				Nodes.Add(MakeShareable(new FJsonValueObject(NodeToJson(Node))));
			}
			EventObj->SetArrayField(TEXT("nodes"), Nodes);

			// Connections
			TArray<TSharedPtr<FJsonValue>> Connections;
			for (const FBlueprintConnectionData& Conn : Event.Connections)
			{
				Connections.Add(MakeShareable(new FJsonValueObject(ConnectionToJson(Conn))));
			}
			EventObj->SetArrayField(TEXT("connections"), Connections);

			Events.Add(MakeShareable(new FJsonValueObject(EventObj)));
		}
		Json->SetArrayField(TEXT("events"), Events);

		// Components
		TArray<TSharedPtr<FJsonValue>> Components;
		for (const FBlueprintComponentData& Comp : Data.Components)
		{
			TSharedPtr<FJsonObject> CompObj = MakeShareable(new FJsonObject);
			CompObj->SetStringField(TEXT("name"), Comp.ComponentName);
			CompObj->SetStringField(TEXT("class"), Comp.ComponentClass);
			CompObj->SetStringField(TEXT("parent"), Comp.ParentComponent);
			CompObj->SetBoolField(TEXT("is_root"), Comp.bIsRootComponent);
			Components.Add(MakeShareable(new FJsonValueObject(CompObj)));
		}
		Json->SetArrayField(TEXT("components"), Components);
	}
	else
	{
		// Just counts for compact summary
		Json->SetNumberField(TEXT("function_count"), Data.Functions.Num());
		Json->SetNumberField(TEXT("event_count"), Data.EventGraph.Num());
		Json->SetNumberField(TEXT("component_count"), Data.Components.Num());
	}

	return Json;
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::CppUsageToJson(const FBlueprintCppFunctionUsage& Usage)
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject);
	Json->SetStringField(TEXT("function_name"), Usage.FunctionName);
	Json->SetStringField(TEXT("function_class"), Usage.FunctionClass);
	Json->SetStringField(TEXT("blueprint_path"), Usage.BlueprintPath);
	Json->SetStringField(TEXT("node_guid"), Usage.NodeGuid);
	Json->SetStringField(TEXT("graph_name"), Usage.GraphName);
	Json->SetBoolField(TEXT("is_blueprint_callable"), Usage.bIsBlueprintCallable);
	Json->SetBoolField(TEXT("is_native_event"), Usage.bIsBlueprintNativeEvent);
	Json->SetBoolField(TEXT("is_implementable_event"), Usage.bIsBlueprintImplementableEvent);
	Json->SetBoolField(TEXT("is_implementation"), Usage.bIsImplementation);
	return Json;
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::ReferenceToJson(const FBlueprintReferenceData& Reference)
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject);
	Json->SetStringField(TEXT("reference_path"), Reference.ReferencePath);
	Json->SetStringField(TEXT("reference_type"), Reference.ReferenceType);
	Json->SetBoolField(TEXT("is_hard_reference"), Reference.bIsHardReference);
	Json->SetStringField(TEXT("context"), Reference.Context);
	return Json;
}

void UBlueprintExportCommandlet::OutputText(const FString& Text)
{
	if (OutputFilePath.IsEmpty())
	{
		// Output to stdout with markers for parsing
		UE_LOG(LogTemp, Display, TEXT("%s%s%s"), JSON_START_MARKER, *Text, JSON_END_MARKER);
	}
	else
	{
		// Write to file
		if (FFileHelper::SaveStringToFile(Text, *OutputFilePath))
		{
			UE_LOG(LogTemp, Display, TEXT("Output written to: %s"), *OutputFilePath);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to write output to: %s"), *OutputFilePath);
		}
	}
}

//------------------------------------------------------------------------------
// Compact Pseudocode Output
//------------------------------------------------------------------------------

FString UBlueprintExportCommandlet::NodesToCompact(const TArray<FBlueprintNodeData>& Nodes, const TArray<FBlueprintConnectionData>& Connections)
{
	FString Result;

	// Build a map of node guid to node for quick lookup
	TMap<FString, const FBlueprintNodeData*> NodeMap;
	for (const FBlueprintNodeData& Node : Nodes)
	{
		NodeMap.Add(Node.NodeGuid, &Node);
	}

	// Build execution flow from connections (track "then" pin connections)
	TMap<FString, FString> ExecFlow; // SourceGuid -> TargetGuid
	TMap<FString, TArray<TPair<FString, FString>>> DataFlow; // TargetGuid -> [(SourceGuid.Pin, TargetPin)]

	for (const FBlueprintConnectionData& Conn : Connections)
	{
		if (Conn.SourcePinName == TEXT("then") || Conn.SourcePinName == TEXT("execute"))
		{
			ExecFlow.Add(Conn.SourceNodeGuid, Conn.TargetNodeGuid);
		}
		else
		{
			DataFlow.FindOrAdd(Conn.TargetNodeGuid).Add(TPair<FString, FString>(
				FString::Printf(TEXT("%s.%s"), *Conn.SourceNodeGuid, *Conn.SourcePinName),
				Conn.TargetPinName));
		}
	}

	// Find entry point (node with no incoming exec connection)
	TSet<FString> HasIncomingExec;
	for (const auto& Pair : ExecFlow)
	{
		HasIncomingExec.Add(Pair.Value);
	}

	const FBlueprintNodeData* CurrentNode = nullptr;
	for (const FBlueprintNodeData& Node : Nodes)
	{
		if (!HasIncomingExec.Contains(Node.NodeGuid))
		{
			// Check if this is an entry/event node
			if (Node.NodeType.Contains(TEXT("Event")) || Node.NodeType.Contains(TEXT("Entry")))
			{
				CurrentNode = &Node;
				break;
			}
		}
	}

	if (!CurrentNode)
	{
		// Fall back to first node
		if (Nodes.Num() > 0)
		{
			CurrentNode = &Nodes[0];
		}
	}

	// Walk the execution flow
	TSet<FString> Visited;
	while (CurrentNode && !Visited.Contains(CurrentNode->NodeGuid))
	{
		Visited.Add(CurrentNode->NodeGuid);

		// Format the node
		FString NodeLine = TEXT("  ");

		// Skip entry/event/reroute nodes in output
		if (!CurrentNode->NodeType.Contains(TEXT("Entry")) && !CurrentNode->NodeType.Contains(TEXT("Event")) && !ShouldSkipNode(*CurrentNode))
		{
			// Check for variable get/set
			if (CurrentNode->NodeType.Contains(TEXT("VariableSet")))
			{
				// Find what's being assigned
				FString VarName = CurrentNode->NodeTitle;
				VarName.RemoveFromStart(TEXT("Set "));
				NodeLine += FString::Printf(TEXT("SET %s"), *VarName);

				// Find input value
				if (const TArray<TPair<FString, FString>>* Inputs = DataFlow.Find(CurrentNode->NodeGuid))
				{
					for (const auto& Input : *Inputs)
					{
						if (!Input.Value.Contains(TEXT("execute")))
						{
							// Try to resolve the source
							FString SourceGuid = Input.Key;
							int32 DotIndex;
							if (SourceGuid.FindChar('.', DotIndex))
							{
								FString Guid = SourceGuid.Left(DotIndex);
								FString Pin = SourceGuid.Mid(DotIndex + 1);
								if (const FBlueprintNodeData** SourceNode = NodeMap.Find(Guid))
								{
									NodeLine += FString::Printf(TEXT(" = %s.%s"), *(*SourceNode)->NodeTitle, *Pin);
								}
							}
						}
					}
				}
			}
			else if (CurrentNode->NodeType.Contains(TEXT("VariableGet")))
			{
				// Variable gets are usually inline, skip standalone
			}
			else if (CurrentNode->NodeType.Contains(TEXT("CallFunction")))
			{
				// Function call
				FString FuncDisplay = CurrentNode->FunctionName.IsEmpty() ? CurrentNode->NodeTitle : CurrentNode->FunctionName;

				// Check if it's a method call (has target)
				FString Target;
				TArray<FString> Args;

				if (const TArray<TPair<FString, FString>>* Inputs = DataFlow.Find(CurrentNode->NodeGuid))
				{
					for (const auto& Input : *Inputs)
					{
						if (Input.Value == TEXT("self") || Input.Value == TEXT("Target"))
						{
							FString SourceGuid = Input.Key;
							int32 DotIndex;
							if (SourceGuid.FindChar('.', DotIndex))
							{
								FString Guid = SourceGuid.Left(DotIndex);
								if (const FBlueprintNodeData** SourceNode = NodeMap.Find(Guid))
								{
									// Skip reroute nodes as targets
									if (!ShouldSkipNode(**SourceNode))
									{
										Target = (*SourceNode)->NodeTitle;
									}
								}
							}
						}
						else if (!Input.Value.Contains(TEXT("execute")))
						{
							Args.Add(Input.Value);
						}
					}
				}

				if (!Target.IsEmpty())
				{
					NodeLine += FString::Printf(TEXT("%s.%s(%s)"), *Target, *FuncDisplay, *FString::Join(Args, TEXT(", ")));
				}
				else
				{
					NodeLine += FString::Printf(TEXT("%s(%s)"), *FuncDisplay, *FString::Join(Args, TEXT(", ")));
				}

				// Note if it's a native C++ function
				if (CurrentNode->bIsNativeFunction)
				{
					NodeLine += TEXT("  // C++");
				}
			}
			else if (CurrentNode->NodeType.Contains(TEXT("Branch")))
			{
				NodeLine += TEXT("Branch:");
				// TODO: Handle branching paths
			}
			else if (CurrentNode->NodeType.Contains(TEXT("Return")))
			{
				NodeLine += TEXT("Return");
			}
			else
			{
				// Generic node
				NodeLine += CurrentNode->NodeTitle;
			}

			if (!NodeLine.TrimStartAndEnd().IsEmpty() && NodeLine.TrimStartAndEnd() != TEXT(""))
			{
				Result += NodeLine + TEXT("\n");
			}
		}

		// Move to next node in execution flow
		if (const FString* NextGuid = ExecFlow.Find(CurrentNode->NodeGuid))
		{
			CurrentNode = NodeMap.FindRef(*NextGuid);
		}
		else
		{
			CurrentNode = nullptr;
		}
	}

	return Result;
}

FString UBlueprintExportCommandlet::FunctionToCompact(const FBlueprintFunctionData& Func)
{
	FString Result;

	// Function signature
	Result += TEXT("Function ") + Func.FunctionName;

	// Inputs
	if (Func.Inputs.Num() > 0)
	{
		Result += TEXT("(");
		TArray<FString> InputStrs;
		for (const FBlueprintPinData& Pin : Func.Inputs)
		{
			InputStrs.Add(FString::Printf(TEXT("%s: %s"), *Pin.PinName, *Pin.PinType));
		}
		Result += FString::Join(InputStrs, TEXT(", "));
		Result += TEXT(")");
	}
	else
	{
		Result += TEXT("()");
	}

	// Outputs
	if (Func.Outputs.Num() > 0)
	{
		Result += TEXT(" -> (");
		TArray<FString> OutputStrs;
		for (const FBlueprintPinData& Pin : Func.Outputs)
		{
			OutputStrs.Add(FString::Printf(TEXT("%s: %s"), *Pin.PinName, *Pin.PinType));
		}
		Result += FString::Join(OutputStrs, TEXT(", "));
		Result += TEXT(")");
	}

	// Flags
	TArray<FString> Flags;
	if (Func.bIsPure) Flags.Add(TEXT("pure"));
	if (Func.bIsConst) Flags.Add(TEXT("const"));
	if (Func.bIsStatic) Flags.Add(TEXT("static"));
	if (Func.bIsOverride) Flags.Add(TEXT("override"));
	if (Flags.Num() > 0)
	{
		Result += TEXT(" [") + FString::Join(Flags, TEXT(", ")) + TEXT("]");
	}

	Result += TEXT("\n");

	// Body
	Result += NodesToCompact(Func.Nodes, Func.Connections);

	return Result;
}

FString UBlueprintExportCommandlet::EventToCompact(const FBlueprintEventData& Event)
{
	FString Result;

	// Event header
	Result += TEXT("Event ") + Event.EventName;
	if (!Event.EventType.IsEmpty() && Event.EventType != TEXT("Event"))
	{
		Result += TEXT(" [") + Event.EventType + TEXT("]");
	}
	Result += TEXT("\n");

	// Body
	Result += NodesToCompact(Event.Nodes, Event.Connections);

	return Result;
}

FString UBlueprintExportCommandlet::BlueprintToCompact(const FBlueprintExportData& Data)
{
	FString Result;

	// Header
	Result += TEXT("# Blueprint: ") + Data.BlueprintName + TEXT("\n");
	Result += TEXT("# Path: ") + Data.BlueprintPath + TEXT("\n");
	Result += TEXT("# Parent: ") + Data.ParentClass + TEXT("\n");
	Result += TEXT("\n");

	// Variables
	if (Data.Variables.Num() > 0)
	{
		Result += TEXT("## Variables\n");
		for (const FBlueprintVariableData& Var : Data.Variables)
		{
			Result += TEXT("  ") + Var.VariableName + TEXT(": ") + Var.VariableType;
			TArray<FString> Flags;
			if (Var.bIsPublic) Flags.Add(TEXT("public"));
			if (Var.bIsReplicated) Flags.Add(TEXT("replicated"));
			if (Flags.Num() > 0)
			{
				Result += TEXT(" [") + FString::Join(Flags, TEXT(", ")) + TEXT("]");
			}
			Result += TEXT("\n");
		}
		Result += TEXT("\n");
	}

	// Functions
	if (Data.Functions.Num() > 0)
	{
		Result += TEXT("## Functions\n");
		for (const FBlueprintFunctionData& Func : Data.Functions)
		{
			Result += FunctionToCompact(Func);
			Result += TEXT("\n");
		}
	}

	// Events
	if (Data.EventGraph.Num() > 0)
	{
		Result += TEXT("## Events\n");
		for (const FBlueprintEventData& Event : Data.EventGraph)
		{
			Result += EventToCompact(Event);
			Result += TEXT("\n");
		}
	}

	// Components
	if (Data.Components.Num() > 0)
	{
		Result += TEXT("## Components\n");
		for (const FBlueprintComponentData& Comp : Data.Components)
		{
			Result += TEXT("  ") + Comp.ComponentName + TEXT(": ") + Comp.ComponentClass;
			if (!Comp.ParentComponent.IsEmpty())
			{
				Result += TEXT(" (parent: ") + Comp.ParentComponent + TEXT(")");
			}
			Result += TEXT("\n");
		}
	}

	return Result;
}

//------------------------------------------------------------------------------
// C++ Skeleton Output
//------------------------------------------------------------------------------

FString UBlueprintExportCommandlet::FunctionToSkeleton(const FBlueprintFunctionData& Func, const FString& ClassName)
{
	FString Result;

	// Determine return type and out params
	FString ReturnType = TEXT("void");
	TArray<FString> OutParams;
	TArray<FString> InParams;

	for (const FBlueprintPinData& Pin : Func.Inputs)
	{
		FString CppType = BPTypeToCppType(Pin.PinType);
		InParams.Add(FString::Printf(TEXT("%s %s"), *CppType, *Pin.PinName));
	}

	for (const FBlueprintPinData& Pin : Func.Outputs)
	{
		FString CppType = BPTypeToCppType(Pin.PinType);

		if (Pin.PinName == TEXT("ReturnValue"))
		{
			ReturnType = CppType;
		}
		else
		{
			OutParams.Add(FString::Printf(TEXT("%s& %s"), *CppType, *Pin.PinName));
		}
	}

	// Combine params
	TArray<FString> AllParams;
	AllParams.Append(InParams);
	AllParams.Append(OutParams);

	// Function signature
	Result += FString::Printf(TEXT("%s %s::%s(%s)"),
		*ReturnType,
		*ClassName,
		*Func.FunctionName,
		*FString::Join(AllParams, TEXT(", ")));

	if (Func.bIsConst)
	{
		Result += TEXT(" const");
	}

	Result += TEXT("\n{\n");

	// Add BP logic as comments
	for (const FBlueprintNodeData& Node : Func.Nodes)
	{
		// Skip entry/result/reroute nodes
		if (Node.NodeType.Contains(TEXT("Entry")) || Node.NodeType.Contains(TEXT("Result")))
		{
			continue;
		}
		if (ShouldSkipNode(Node))
		{
			continue;
		}

		Result += TEXT("\t// BP: ") + Node.NodeTitle;
		if (!Node.FunctionName.IsEmpty() && Node.bIsNativeFunction)
		{
			Result += TEXT(" -> ") + Node.FunctionClass + TEXT("::") + Node.FunctionName + TEXT("()");
		}
		Result += TEXT("\n");
	}

	// Placeholder return
	if (ReturnType != TEXT("void"))
	{
		Result += TEXT("\n\treturn {}; // TODO\n");
	}

	Result += TEXT("}\n");

	return Result;
}

FString UBlueprintExportCommandlet::EventToSkeleton(const FBlueprintEventData& Event, const FString& ClassName)
{
	FString Result;

	// Clean up event name for C++ function name
	FString FuncName = CleanEventName(Event.EventName);

	// Check if this is likely an override
	bool bIsOverride = Event.EventType == TEXT("NativeEvent") || Event.EventType == TEXT("ImplementableEvent");

	Result += FString::Printf(TEXT("void %s::%s()"), *ClassName, *FuncName);
	if (bIsOverride)
	{
		Result += TEXT(" // override");
	}
	Result += TEXT("\n{\n");

	// Add BP logic as comments
	for (const FBlueprintNodeData& Node : Event.Nodes)
	{
		// Skip event/reroute nodes
		if (Node.NodeType.Contains(TEXT("Event")))
		{
			continue;
		}
		if (ShouldSkipNode(Node))
		{
			continue;
		}

		Result += TEXT("\t// BP: ") + Node.NodeTitle;
		if (!Node.FunctionName.IsEmpty() && Node.bIsNativeFunction)
		{
			Result += TEXT(" -> ") + Node.FunctionClass + TEXT("::") + Node.FunctionName + TEXT("()");
		}
		Result += TEXT("\n");
	}

	Result += TEXT("}\n");

	return Result;
}

FString UBlueprintExportCommandlet::BlueprintToSkeleton(const FBlueprintExportData& Data)
{
	FString Result;

	// Determine class name
	FString ClassName = TEXT("A") + Data.BlueprintName;
	ClassName.ReplaceInline(TEXT("_BP"), TEXT(""));
	ClassName.ReplaceInline(TEXT("_C"), TEXT(""));

	// Header comment
	Result += TEXT("// =============================================================================\n");
	Result += TEXT("// C++ Skeleton generated from Blueprint\n");
	Result += TEXT("// Source: ") + Data.BlueprintPath + TEXT("\n");
	Result += TEXT("// Parent: ") + Data.ParentClass + TEXT("\n");
	Result += TEXT("// =============================================================================\n\n");

	// Collect required includes based on types
	TSet<FString> Includes;
	for (const FBlueprintVariableData& Var : Data.Variables)
	{
		if (Var.VariableType.Contains(TEXT("object<")))
		{
			FString TypeName = Var.VariableType;
			int32 Start = TypeName.Find(TEXT("<"));
			int32 End = TypeName.Find(TEXT(">"));
			if (Start != INDEX_NONE && End != INDEX_NONE)
			{
				Includes.Add(TypeName.Mid(Start + 1, End - Start - 1));
			}
		}
	}

	if (Includes.Num() > 0)
	{
		Result += TEXT("// Required includes (verify paths):\n");
		for (const FString& Inc : Includes)
		{
			Result += TEXT("// #include \"") + Inc + TEXT(".h\"\n");
		}
		Result += TEXT("\n");
	}

	// Class declaration snippet
	Result += TEXT("// --- Header snippet ---\n");
	Result += TEXT("UCLASS()\n");
	Result += FString::Printf(TEXT("class %s : public %s\n"), *ClassName, *Data.ParentClass);
	Result += TEXT("{\n");
	Result += TEXT("\tGENERATED_BODY()\n\n");
	Result += TEXT("public:\n");

	// Variables
	for (const FBlueprintVariableData& Var : Data.Variables)
	{
		FString CppType = BPTypeToCppType(Var.VariableType);

		Result += TEXT("\tUPROPERTY(");
		TArray<FString> Specifiers;
		if (Var.bIsPublic) Specifiers.Add(TEXT("BlueprintReadWrite"));
		else Specifiers.Add(TEXT("BlueprintReadOnly"));
		if (Var.bIsReplicated) Specifiers.Add(TEXT("Replicated"));
		Specifiers.Add(TEXT("Category = \"Default\""));
		Result += FString::Join(Specifiers, TEXT(", "));
		Result += TEXT(")\n");
		Result += FString::Printf(TEXT("\t%s %s;\n\n"), *CppType, *Var.VariableName);
	}

	// Function declarations
	for (const FBlueprintFunctionData& Func : Data.Functions)
	{
		Result += TEXT("\tUFUNCTION(BlueprintCallable)\n");

		FString ReturnType = TEXT("void");
		for (const FBlueprintPinData& Pin : Func.Outputs)
		{
			if (Pin.PinName == TEXT("ReturnValue"))
			{
				ReturnType = BPTypeToCppType(Pin.PinType);
				break;
			}
		}

		Result += FString::Printf(TEXT("\t%s %s(...);\n\n"), *ReturnType, *Func.FunctionName);
	}

	Result += TEXT("};\n\n");

	// Implementation
	Result += TEXT("// --- Implementation ---\n\n");

	for (const FBlueprintFunctionData& Func : Data.Functions)
	{
		Result += FunctionToSkeleton(Func, ClassName);
		Result += TEXT("\n");
	}

	for (const FBlueprintEventData& Event : Data.EventGraph)
	{
		Result += EventToSkeleton(Event, ClassName);
		Result += TEXT("\n");
	}

	return Result;
}
