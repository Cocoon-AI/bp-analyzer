// BlueprintExportReader.cpp
// Implementation for Blueprint data extraction

#include "BlueprintExportReader.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Script.h"
#include "Engine/Blueprint.h"

// UE4.27 compatibility - these flags don't exist separately,
// BlueprintNativeEvent = has FUNC_BlueprintEvent AND FUNC_Native
// BlueprintImplementableEvent = has FUNC_BlueprintEvent but NOT FUNC_Native
static bool IsBlueprintNativeEvent(const UFunction* Function)
{
	return Function && Function->HasAnyFunctionFlags(FUNC_BlueprintEvent) && Function->HasAnyFunctionFlags(FUNC_Native);
}
static bool IsBlueprintImplementableEvent(const UFunction* Function)
{
	return Function && Function->HasAnyFunctionFlags(FUNC_BlueprintEvent) && !Function->HasAnyFunctionFlags(FUNC_Native);
}
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Tunnel.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryState.h"
#include "UObject/UObjectIterator.h"
#include "Misc/PackageName.h"

// Returns true when the pin type references a sub-object (struct, object, class,
// enum, etc.) but that object can no longer be resolved — typically because the
// backing USTRUCT / UClass was deleted from C++.
static bool IsPinTypeBroken(const FEdGraphPinType& PinType)
{
	// These categories require a valid PinSubCategoryObject to be meaningful.
	static const TSet<FName> NeedsSubObject = {
		UEdGraphSchema_K2::PC_Struct,
		UEdGraphSchema_K2::PC_Object,
		UEdGraphSchema_K2::PC_Class,
		UEdGraphSchema_K2::PC_SoftObject,
		UEdGraphSchema_K2::PC_SoftClass,
		UEdGraphSchema_K2::PC_Interface,
		UEdGraphSchema_K2::PC_Enum,
	};
	return NeedsSubObject.Contains(PinType.PinCategory) && !PinType.PinSubCategoryObject.IsValid();
}

// Returns true if any pin on the node has a broken type reference.
// Used to guard against calling GetNodeTitle on broken struct nodes.
static bool IsPinTypeBroken_AnyPin(const UEdGraphNode* Node)
{
	if (!Node) { return false; }
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && IsPinTypeBroken(Pin->PinType)) { return true; }
	}
	return false;
}

// Helper to convert FEdGraphPinType to string
static FString PinTypeToString(const FEdGraphPinType& PinType)
{
	FString Result = PinType.PinCategory.ToString();

	if (PinType.PinSubCategory != NAME_None)
	{
		Result += TEXT(":") + PinType.PinSubCategory.ToString();
	}

	if (PinType.PinSubCategoryObject.IsValid())
	{
		Result += TEXT("<") + PinType.PinSubCategoryObject->GetName() + TEXT(">");
	}

	if (PinType.ContainerType == EPinContainerType::Array)
	{
		Result = TEXT("TArray<") + Result + TEXT(">");
	}
	else if (PinType.ContainerType == EPinContainerType::Set)
	{
		Result = TEXT("TSet<") + Result + TEXT(">");
	}
	else if (PinType.ContainerType == EPinContainerType::Map)
	{
		Result = TEXT("TMap<") + Result + TEXT(">");
	}

	if (PinType.bIsReference)
	{
		Result += TEXT("&");
	}

	if (PinType.bIsConst)
	{
		Result = TEXT("const ") + Result;
	}

	return Result;
}

// Helper to convert EBlueprintType to string
static FString BlueprintTypeToString(EBlueprintType Type)
{
	switch (Type)
	{
		case BPTYPE_Normal: return TEXT("Blueprint");
		case BPTYPE_Const: return TEXT("ConstBlueprint");
		case BPTYPE_MacroLibrary: return TEXT("MacroLibrary");
		case BPTYPE_Interface: return TEXT("Interface");
		case BPTYPE_LevelScript: return TEXT("LevelScript");
		case BPTYPE_FunctionLibrary: return TEXT("FunctionLibrary");
		default: return TEXT("Unknown");
	}
}

// Extract pin data from an EdGraphPin
static FBlueprintPinData ExtractPinData(const UEdGraphPin* Pin)
{
	FBlueprintPinData PinData;

	if (!Pin) return PinData;

	PinData.PinName = Pin->PinName.ToString();
	PinData.PinType = PinTypeToString(Pin->PinType);
	PinData.DefaultValue = Pin->DefaultValue;
	PinData.bIsArray = Pin->PinType.ContainerType == EPinContainerType::Array;
	PinData.bIsReference = Pin->PinType.bIsReference;
	PinData.bIsConst = Pin->PinType.bIsConst;

	// Get linked pin info
	if (Pin->LinkedTo.Num() > 0)
	{
		TArray<FString> LinkedPins;
		for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (LinkedPin && LinkedPin->GetOwningNode())
			{
				LinkedPins.Add(FString::Printf(TEXT("%s.%s"),
					*LinkedPin->GetOwningNode()->NodeGuid.ToString(),
					*LinkedPin->PinName.ToString()));
			}
		}
		PinData.LinkedTo = FString::Join(LinkedPins, TEXT(","));
	}

	return PinData;
}

// Extract node data from a K2Node
static FBlueprintNodeData ExtractNodeData(const UK2Node* K2Node)
{
	FBlueprintNodeData NodeData;

	if (!K2Node) return NodeData;

	NodeData.NodeGuid = K2Node->NodeGuid.ToString();
	NodeData.NodeType = K2Node->GetClass()->GetName();
	NodeData.NodeTitle = K2Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	NodeData.NodeComment = K2Node->NodeComment;
	NodeData.PositionX = K2Node->NodePosX;
	NodeData.PositionY = K2Node->NodePosY;

	// Extract input pins
	for (const UEdGraphPin* Pin : K2Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input)
		{
			NodeData.InputPins.Add(ExtractPinData(Pin));
		}
	}

	// Extract output pins
	for (const UEdGraphPin* Pin : K2Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output)
		{
			NodeData.OutputPins.Add(ExtractPinData(Pin));
		}
	}

	// Check if this is a function call node
	if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(K2Node))
	{
		if (UFunction* Function = CallNode->GetTargetFunction())
		{
			NodeData.FunctionName = Function->GetName();
			NodeData.FunctionClass = Function->GetOwnerClass() ? Function->GetOwnerClass()->GetName() : TEXT("");
			NodeData.bIsBlueprintCallable = Function->HasAnyFunctionFlags(FUNC_BlueprintCallable);
			NodeData.bIsBlueprintNativeEvent = IsBlueprintNativeEvent(Function);
			NodeData.bIsBlueprintImplementableEvent = IsBlueprintImplementableEvent(Function);
			NodeData.bIsNativeFunction = Function->HasAnyFunctionFlags(FUNC_Native);
		}
	}

	return NodeData;
}

// Extract connections from a graph
static TArray<FBlueprintConnectionData> ExtractConnections(const UEdGraph* Graph)
{
	TArray<FBlueprintConnectionData> Connections;

	if (!Graph) return Connections;

	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output) continue;

			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;

				FBlueprintConnectionData Connection;
				Connection.SourceNodeGuid = Node->NodeGuid.ToString();
				Connection.SourcePinName = Pin->PinName.ToString();
				Connection.TargetNodeGuid = LinkedPin->GetOwningNode()->NodeGuid.ToString();
				Connection.TargetPinName = LinkedPin->PinName.ToString();
				Connections.Add(Connection);
			}
		}
	}

	return Connections;
}

// Extract all nodes from a graph
static TArray<FBlueprintNodeData> ExtractNodes(const UEdGraph* Graph)
{
	TArray<FBlueprintNodeData> Nodes;

	if (!Graph) return Nodes;

	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (const UK2Node* K2Node = Cast<UK2Node>(Node))
		{
			Nodes.Add(ExtractNodeData(K2Node));
		}
	}

	return Nodes;
}

FBlueprintExportData UBlueprintExportReader::ExportBlueprint(const FString& BlueprintPath)
{
	FBlueprintExportData ExportData;

	// Load the blueprint
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!Blueprint)
	{
		// Try with _C suffix for the class
		FString ClassPath = BlueprintPath;
		if (!ClassPath.EndsWith(TEXT("_C")))
		{
			ClassPath += TEXT("_C");
		}

		// Try loading as a blueprint generated class and find its blueprint
		UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath);
		if (LoadedClass)
		{
			Blueprint = Cast<UBlueprint>(LoadedClass->ClassGeneratedBy);
		}
	}

	if (!Blueprint)
	{
		return ExportData;
	}

	return ExportBlueprintObject(Blueprint);
}

FBlueprintExportData UBlueprintExportReader::ExportBlueprintObject(UBlueprint* Blueprint)
{
	FBlueprintExportData ExportData;

	if (!Blueprint)
	{
		return ExportData;
	}

	// Basic info
	ExportData.BlueprintName = Blueprint->GetName();
	ExportData.BlueprintPath = Blueprint->GetPathName();
	ExportData.BlueprintType = BlueprintTypeToString(Blueprint->BlueprintType);

	// Parent class
	if (Blueprint->ParentClass)
	{
		ExportData.ParentClass = Blueprint->ParentClass->GetName();
	}

	// Blueprint flags
	ExportData.bIsDataOnly = Blueprint->bGenerateConstClass;
	ExportData.bIsInterface = Blueprint->BlueprintType == BPTYPE_Interface;
	ExportData.bIsAbstract = Blueprint->bGenerateAbstractClass;
	ExportData.bIsDeprecated = Blueprint->bDeprecate;

#if WITH_EDITORONLY_DATA
	ExportData.BlueprintDescription = Blueprint->BlueprintDescription;
	ExportData.BlueprintCategory = Blueprint->BlueprintCategory;

	// Implemented interfaces
	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		if (Interface.Interface)
		{
			ExportData.ImplementedInterfaces.Add(Interface.Interface->GetName());
		}
	}

	// Variables
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		FBlueprintVariableData VarData;
		VarData.VariableName = Var.VarName.ToString();
		VarData.VariableType = PinTypeToString(Var.VarType);
		VarData.bIsTypeBroken = IsPinTypeBroken(Var.VarType);
		VarData.Category = Var.Category.ToString();
		VarData.DefaultValue = Var.DefaultValue;
		VarData.bIsPublic = (Var.PropertyFlags & CPF_BlueprintVisible) != 0;
		VarData.bIsReadOnly = (Var.PropertyFlags & CPF_BlueprintReadOnly) != 0;
		VarData.bIsBlueprintVisible = (Var.PropertyFlags & CPF_BlueprintVisible) != 0;
		VarData.bIsReplicated = (Var.PropertyFlags & CPF_Net) != 0;

		if (Var.ReplicationCondition != COND_None)
		{
			VarData.ReplicationCondition = StaticEnum<ELifetimeCondition>()->GetNameStringByValue((int64)Var.ReplicationCondition);
		}

		// Get tooltip from metadata
		int32 TooltipIndex = Var.FindMetaDataEntryIndexForKey(TEXT("tooltip"));
		if (TooltipIndex != INDEX_NONE)
		{
			VarData.ToolTip = Var.MetaDataArray[TooltipIndex].DataValue;
		}

		ExportData.Variables.Add(VarData);
	}

	// Function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph) continue;

		FBlueprintFunctionData FuncData;
		FuncData.FunctionName = Graph->GetName();
		FuncData.Nodes = ExtractNodes(Graph);
		FuncData.Connections = ExtractConnections(Graph);

		// Find function entry node for inputs/outputs
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
			{
				for (UEdGraphPin* Pin : EntryNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output &&
						Pin->PinName != UEdGraphSchema_K2::PN_Then &&
						Pin->PinName != UEdGraphSchema_K2::PN_Execute)
					{
						FuncData.Inputs.Add(ExtractPinData(Pin));
					}
				}

				// Get function flags from the entry node
				if (UFunction* Function = EntryNode->FindSignatureFunction())
				{
					FuncData.bIsPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
					FuncData.bIsStatic = Function->HasAnyFunctionFlags(FUNC_Static);
					FuncData.bIsConst = Function->HasAnyFunctionFlags(FUNC_Const);

					const FString* CategoryMeta = Function->FindMetaData(TEXT("Category"));
					if (CategoryMeta && !CategoryMeta->IsEmpty())
					{
						FuncData.Category = *CategoryMeta;
					}
				}
			}
			else if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
			{
				for (UEdGraphPin* Pin : ResultNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input &&
						Pin->PinName != UEdGraphSchema_K2::PN_Execute)
					{
						FuncData.Outputs.Add(ExtractPinData(Pin));
					}
				}
			}
		}

		// Check if override
		if (Blueprint->ParentClass)
		{
			UFunction* ParentFunc = Blueprint->ParentClass->FindFunctionByName(FName(*FuncData.FunctionName));
			FuncData.bIsOverride = (ParentFunc != nullptr);
		}

		ExportData.Functions.Add(FuncData);
	}

	// Locally-defined macros. Macro graphs use UK2Node_Tunnel (not FunctionEntry/Result)
	// as their entry and exit nodes. From the tunnel's perspective, output-direction
	// data pins feed INTO the macro body (so they represent the macro's formal inputs),
	// and input-direction pins receive FROM the body (representing formal outputs).
	// Exec pins follow the same shape but we skip them here for parity with the
	// function extraction above — the full pin set is still visible via Nodes.
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (!Graph) continue;

		FBlueprintFunctionData MacroData;
		MacroData.FunctionName = Graph->GetName();
		MacroData.Nodes = ExtractNodes(Graph);
		MacroData.Connections = ExtractConnections(Graph);

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Tunnel* TunnelNode = Cast<UK2Node_Tunnel>(Node);
			if (!TunnelNode) continue;
			// FunctionEntry/FunctionResult inherit from Tunnel via FunctionTerminator —
			// those would indicate a function graph, not a macro, so skip them.
			if (Cast<UK2Node_FunctionEntry>(Node) || Cast<UK2Node_FunctionResult>(Node)) continue;

			for (UEdGraphPin* Pin : TunnelNode->Pins)
			{
				if (!Pin) continue;
				if (Pin->PinName == UEdGraphSchema_K2::PN_Then ||
					Pin->PinName == UEdGraphSchema_K2::PN_Execute)
				{
					continue;
				}
				if (Pin->Direction == EGPD_Output)
				{
					MacroData.Inputs.Add(ExtractPinData(Pin));
				}
				else if (Pin->Direction == EGPD_Input)
				{
					MacroData.Outputs.Add(ExtractPinData(Pin));
				}
			}
		}

		ExportData.Macros.Add(MacroData);
	}

	// Event graphs (Ubergraph pages)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph) continue;

		// Find event nodes in the graph
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
			if (!EventNode) continue;

			FBlueprintEventData EventData;
			EventData.EventName = EventNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

			// Determine event type
			if (Cast<UK2Node_CustomEvent>(EventNode))
			{
				EventData.EventType = TEXT("Custom");
			}
			else
			{
				// Check if it's a native event implementation
				if (UFunction* Function = EventNode->FindEventSignatureFunction())
				{
					if (IsBlueprintNativeEvent(Function))
					{
						EventData.EventType = TEXT("NativeEvent");
					}
					else if (IsBlueprintImplementableEvent(Function))
					{
						EventData.EventType = TEXT("ImplementableEvent");
					}
					else
					{
						EventData.EventType = TEXT("Event");
					}
				}
				else
				{
					EventData.EventType = TEXT("Event");
				}
			}

			// Get connected nodes (traverse from this event)
			TSet<UEdGraphNode*> VisitedNodes;
			TArray<UEdGraphNode*> NodesToProcess;
			NodesToProcess.Add(EventNode);

			while (NodesToProcess.Num() > 0)
			{
				UEdGraphNode* CurrentNode = NodesToProcess.Pop();
				if (VisitedNodes.Contains(CurrentNode)) continue;
				VisitedNodes.Add(CurrentNode);

				if (UK2Node* K2Node = Cast<UK2Node>(CurrentNode))
				{
					EventData.Nodes.Add(ExtractNodeData(K2Node));
				}

				// Add connected nodes
				for (UEdGraphPin* Pin : CurrentNode->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output) continue;

					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode())
						{
							NodesToProcess.Add(LinkedPin->GetOwningNode());
						}
					}
				}
			}

			// Extract connections for visited nodes
			for (UEdGraphNode* VisitedNode : VisitedNodes)
			{
				for (UEdGraphPin* Pin : VisitedNode->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output) continue;

					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode() && VisitedNodes.Contains(LinkedPin->GetOwningNode()))
						{
							FBlueprintConnectionData Connection;
							Connection.SourceNodeGuid = VisitedNode->NodeGuid.ToString();
							Connection.SourcePinName = Pin->PinName.ToString();
							Connection.TargetNodeGuid = LinkedPin->GetOwningNode()->NodeGuid.ToString();
							Connection.TargetPinName = LinkedPin->PinName.ToString();
							EventData.Connections.Add(Connection);
						}
					}
				}
			}

			ExportData.EventGraph.Add(EventData);
		}
	}

	// Delegate signature graphs (Event Dispatchers)
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (!Graph) continue;

		FBlueprintDispatcherData DispatcherData;
		DispatcherData.DispatcherName = Graph->GetName();

		// Find the delegate signature node
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
			{
				for (UEdGraphPin* Pin : EntryNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output &&
						Pin->PinName != UEdGraphSchema_K2::PN_Then)
					{
						DispatcherData.Parameters.Add(ExtractPinData(Pin));
					}
				}
			}
		}

		ExportData.EventDispatchers.Add(DispatcherData);
	}
#endif // WITH_EDITORONLY_DATA

	// Components from Simple Construction Script
	if (Blueprint->SimpleConstructionScript)
	{
		TArray<USCS_Node*> AllNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
		for (USCS_Node* SCSNode : AllNodes)
		{
			if (!SCSNode || !SCSNode->ComponentTemplate) continue;

			FBlueprintComponentData CompData;
			CompData.ComponentName = SCSNode->GetVariableName().ToString();
			CompData.ComponentClass = SCSNode->ComponentTemplate->GetClass()->GetName();
			CompData.bIsRootComponent = (SCSNode == Blueprint->SimpleConstructionScript->GetDefaultSceneRootNode());

			if (SCSNode->ParentComponentOrVariableName != NAME_None)
			{
				CompData.ParentComponent = SCSNode->ParentComponentOrVariableName.ToString();
			}

			// Get transform if it's a scene component
			if (USceneComponent* SceneComp = Cast<USceneComponent>(SCSNode->ComponentTemplate))
			{
				CompData.RelativeLocation = SceneComp->GetRelativeLocation();
				CompData.RelativeRotation = SceneComp->GetRelativeRotation();
				CompData.RelativeScale3D = SceneComp->GetRelativeScale3D();
			}

			ExportData.Components.Add(CompData);
		}
	}

	return ExportData;
}

TArray<FBlueprintReferenceData> UBlueprintExportReader::GetBlueprintReferences(const FString& BlueprintPath, bool bIncludeSoftReferences)
{
	TArray<FBlueprintReferenceData> References;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Get the asset data for the blueprint
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FName(*BlueprintPath));
	if (!AssetData.IsValid())
	{
		// Try without the path prefix
		if (BlueprintPath.StartsWith(TEXT("/Game/")))
		{
			AssetData = AssetRegistry.GetAssetByObjectPath(FName(*BlueprintPath));
		}
	}

	if (!AssetData.IsValid())
	{
		return References;
	}

	// Get all package dependencies using the new API
	TArray<FAssetDependency> AllDependencies;
	AssetRegistry.GetDependencies(AssetData.PackageName, AllDependencies, UE::AssetRegistry::EDependencyCategory::Package);

	for (const FAssetDependency& Dep : AllDependencies)
	{
		bool bIsHard = (Dep.Properties & UE::AssetRegistry::EDependencyProperty::Hard) != UE::AssetRegistry::EDependencyProperty::None;

		// Skip soft references if not requested
		if (!bIsHard && !bIncludeSoftReferences)
		{
			continue;
		}

		FBlueprintReferenceData RefData;
		RefData.ReferencePath = Dep.AssetId.PackageName.ToString();
		RefData.bIsHardReference = bIsHard;
		RefData.ReferenceType = bIsHard ? TEXT("Dependency") : TEXT("SoftDependency");
		References.Add(RefData);
	}

	return References;
}

TMap<FString, FBlueprintExportData> UBlueprintExportReader::ExportBlueprintGraph(const FString& RootPath, int32 MaxDepth, bool bIncludeSoftReferences)
{
	TMap<FString, FBlueprintExportData> Result;

	// Track what we've processed to avoid cycles
	TSet<FString> ProcessedPaths;
	TArray<TPair<FString, int32>> ToProcess;  // Path and current depth

	ToProcess.Add(TPair<FString, int32>(RootPath, 0));

	while (ToProcess.Num() > 0)
	{
		TPair<FString, int32> Current = ToProcess.Pop();
		FString CurrentPath = Current.Key;
		int32 CurrentDepth = Current.Value;

		if (ProcessedPaths.Contains(CurrentPath) || CurrentDepth > MaxDepth)
		{
			continue;
		}

		ProcessedPaths.Add(CurrentPath);

		// Export this blueprint
		FBlueprintExportData ExportData = ExportBlueprint(CurrentPath);
		if (ExportData.BlueprintName.IsEmpty())
		{
			continue;
		}

		Result.Add(CurrentPath, ExportData);

		// Get references and add to processing queue
		TArray<FBlueprintReferenceData> References = GetBlueprintReferences(CurrentPath, bIncludeSoftReferences);
		for (const FBlueprintReferenceData& Ref : References)
		{
			if (!ProcessedPaths.Contains(Ref.ReferencePath))
			{
				ToProcess.Add(TPair<FString, int32>(Ref.ReferencePath, CurrentDepth + 1));
			}
		}
	}

	return Result;
}

TArray<FBlueprintCppFunctionUsage> UBlueprintExportReader::FindBlueprintsCallingFunction(const FString& FunctionName, const FString& FunctionClass, const TArray<FString>& SearchPaths)
{
	TArray<FBlueprintCppFunctionUsage> Results;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Find all blueprint assets in search paths
	for (const FString& SearchPath : SearchPaths)
	{
		TArray<FAssetData> AssetList;
		AssetRegistry.GetAssetsByPath(FName(*SearchPath), AssetList, true);

		for (const FAssetData& AssetData : AssetList)
		{
			// Check if this is a blueprint
			if (!AssetData.AssetClass.ToString().Contains(TEXT("Blueprint")))
			{
				continue;
			}

			// Load the blueprint
			UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
			if (!Blueprint)
			{
				continue;
			}

			// Search all graphs for function calls
			TArray<UEdGraph*> AllGraphs;
#if WITH_EDITORONLY_DATA
			AllGraphs.Append(Blueprint->FunctionGraphs);
			AllGraphs.Append(Blueprint->UbergraphPages);
			AllGraphs.Append(Blueprint->MacroGraphs);
			AllGraphs.Append(Blueprint->DelegateSignatureGraphs);
#endif

			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph) continue;

				for (UEdGraphNode* Node : Graph->Nodes)
				{
					UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
					if (!CallNode) continue;

					UFunction* Function = CallNode->GetTargetFunction();
					if (!Function) continue;

					// Check if this matches our search
					bool bNameMatches = Function->GetName() == FunctionName;
					bool bClassMatches = FunctionClass.IsEmpty() ||
						(Function->GetOwnerClass() && Function->GetOwnerClass()->GetName() == FunctionClass);

					if (bNameMatches && bClassMatches)
					{
						FBlueprintCppFunctionUsage Usage;
						Usage.FunctionName = Function->GetName();
						Usage.FunctionClass = Function->GetOwnerClass() ? Function->GetOwnerClass()->GetName() : TEXT("");
						Usage.BlueprintPath = Blueprint->GetPathName();
						Usage.NodeGuid = CallNode->NodeGuid.ToString();
						Usage.GraphName = Graph->GetName();
						Usage.bIsBlueprintCallable = Function->HasAnyFunctionFlags(FUNC_BlueprintCallable);
						Usage.bIsBlueprintNativeEvent = IsBlueprintNativeEvent(Function);
						Usage.bIsBlueprintImplementableEvent = IsBlueprintImplementableEvent(Function);
						Usage.bIsImplementation = false;

						Results.Add(Usage);
					}
				}
			}
		}
	}

	return Results;
}

TArray<FBlueprintVariableUsage> UBlueprintExportReader::FindBlueprintsUsingVariable(
	const FString& VariableName,
	const FString& Kind,
	const TArray<FString>& SearchPaths)
{
	TArray<FBlueprintVariableUsage> Results;

	const bool bIncludeGet = Kind.IsEmpty() || Kind.Equals(TEXT("any"), ESearchCase::IgnoreCase) || Kind.Equals(TEXT("get"), ESearchCase::IgnoreCase);
	const bool bIncludeSet = Kind.IsEmpty() || Kind.Equals(TEXT("any"), ESearchCase::IgnoreCase) || Kind.Equals(TEXT("set"), ESearchCase::IgnoreCase);

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	for (const FString& SearchPath : SearchPaths)
	{
		TArray<FAssetData> AssetList;
		AssetRegistry.GetAssetsByPath(FName(*SearchPath), AssetList, true);

		for (const FAssetData& AssetData : AssetList)
		{
			if (!AssetData.AssetClass.ToString().Contains(TEXT("Blueprint")))
			{
				continue;
			}

			UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
			if (!Blueprint)
			{
				continue;
			}

			TArray<UEdGraph*> AllGraphs;
#if WITH_EDITORONLY_DATA
			AllGraphs.Append(Blueprint->FunctionGraphs);
			AllGraphs.Append(Blueprint->UbergraphPages);
			AllGraphs.Append(Blueprint->MacroGraphs);
			AllGraphs.Append(Blueprint->DelegateSignatureGraphs);
#endif

			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph) continue;

				for (UEdGraphNode* Node : Graph->Nodes)
				{
					// K2Node_VariableSet inherits from K2Node_Variable, as does K2Node_VariableGet.
					// Check concrete types so we can tag the access kind.
					FString AccessKind;
					FName VarName;
					UClass* OwningClass = nullptr;

					if (UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(Node))
					{
						if (!bIncludeSet) continue;
						AccessKind = TEXT("set");
						VarName = SetNode->GetVarName();
						// GetMemberParentClass(nullptr) returns the explicit parent class if the
						// reference names one, or nullptr for self-scope vars. That's fine — a
						// self-scope miss just means the BlueprintPath already tells the caller
						// where the var lives.
						OwningClass = SetNode->VariableReference.GetMemberParentClass(nullptr);
					}
					else if (UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(Node))
					{
						if (!bIncludeGet) continue;
						AccessKind = TEXT("get");
						VarName = GetNode->GetVarName();
						OwningClass = GetNode->VariableReference.GetMemberParentClass(nullptr);
					}
					else
					{
						continue;
					}

					if (VarName.ToString() != VariableName)
					{
						continue;
					}

					FBlueprintVariableUsage Usage;
					Usage.VariableName = VarName.ToString();
					Usage.VariableClass = OwningClass ? OwningClass->GetName() : TEXT("");
					Usage.BlueprintPath = Blueprint->GetPathName();
					Usage.NodeGuid = Node->NodeGuid.ToString();
					Usage.GraphName = Graph->GetName();
					Usage.AccessKind = AccessKind;
					Results.Add(Usage);
				}
			}
		}
	}

	return Results;
}

TArray<FBlueprintCppFunctionUsage> UBlueprintExportReader::FindBlueprintNativeEventImplementations(const TArray<FString>& SearchPaths)
{
	TArray<FBlueprintCppFunctionUsage> Results;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	for (const FString& SearchPath : SearchPaths)
	{
		TArray<FAssetData> AssetList;
		AssetRegistry.GetAssetsByPath(FName(*SearchPath), AssetList, true);

		for (const FAssetData& AssetData : AssetList)
		{
			if (!AssetData.AssetClass.ToString().Contains(TEXT("Blueprint")))
			{
				continue;
			}

			UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
			if (!Blueprint)
			{
				continue;
			}

#if WITH_EDITORONLY_DATA
			// Check event graphs for native event implementations
			for (UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				if (!Graph) continue;

				for (UEdGraphNode* Node : Graph->Nodes)
				{
					UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
					if (!EventNode) continue;

					UFunction* Function = EventNode->FindEventSignatureFunction();
					if (!Function) continue;

					if (IsBlueprintNativeEvent(Function))
					{
						FBlueprintCppFunctionUsage Usage;
						Usage.FunctionName = Function->GetName();
						Usage.FunctionClass = Function->GetOwnerClass() ? Function->GetOwnerClass()->GetName() : TEXT("");
						Usage.BlueprintPath = Blueprint->GetPathName();
						Usage.NodeGuid = EventNode->NodeGuid.ToString();
						Usage.GraphName = Graph->GetName();
						Usage.bIsBlueprintCallable = Function->HasAnyFunctionFlags(FUNC_BlueprintCallable);
						Usage.bIsBlueprintNativeEvent = true;
						Usage.bIsBlueprintImplementableEvent = false;
						Usage.bIsImplementation = true;

						Results.Add(Usage);
					}
				}
			}
#endif
		}
	}

	return Results;
}

TArray<FBlueprintCppFunctionUsage> UBlueprintExportReader::FindBlueprintImplementableEventImplementations(
	const FString& EventName,
	const TArray<FString>& SearchPaths)
{
	TArray<FBlueprintCppFunctionUsage> Results;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	for (const FString& SearchPath : SearchPaths)
	{
		TArray<FAssetData> AssetList;
		AssetRegistry.GetAssetsByPath(FName(*SearchPath), AssetList, true);

		for (const FAssetData& AssetData : AssetList)
		{
			if (!AssetData.AssetClass.ToString().Contains(TEXT("Blueprint")))
			{
				continue;
			}

			UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
			if (!Blueprint)
			{
				continue;
			}

#if WITH_EDITORONLY_DATA
			// Check event graphs for implementable event implementations
			for (UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				if (!Graph) continue;

				for (UEdGraphNode* Node : Graph->Nodes)
				{
					UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
					if (!EventNode) continue;

					// Get the event name from the node title
					FString NodeEventName = EventNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

					// Also check the function signature
					UFunction* Function = EventNode->FindEventSignatureFunction();
					FString FunctionEventName = Function ? Function->GetName() : TEXT("");

					// Match against the search event name
					bool bMatches = NodeEventName.Contains(EventName) ||
					                FunctionEventName.Equals(EventName, ESearchCase::IgnoreCase);

					if (bMatches && Function && IsBlueprintImplementableEvent(Function))
					{
						FBlueprintCppFunctionUsage Usage;
						Usage.FunctionName = Function->GetName();
						Usage.FunctionClass = Function->GetOwnerClass() ? Function->GetOwnerClass()->GetName() : TEXT("");
						Usage.BlueprintPath = Blueprint->GetPathName();
						Usage.NodeGuid = EventNode->NodeGuid.ToString();
						Usage.GraphName = Graph->GetName();
						Usage.bIsBlueprintCallable = Function->HasAnyFunctionFlags(FUNC_BlueprintCallable);
						Usage.bIsBlueprintNativeEvent = false;
						Usage.bIsBlueprintImplementableEvent = true;
						Usage.bIsImplementation = true;

						Results.Add(Usage);
					}
				}
			}
#endif
		}
	}

	return Results;
}

TArray<FBlueprintCppFunctionUsage> UBlueprintExportReader::GetBlueprintCppFunctionUsage(const FString& BlueprintPath)
{
	TArray<FBlueprintCppFunctionUsage> Results;

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!Blueprint)
	{
		return Results;
	}

#if WITH_EDITORONLY_DATA
	// Search all graphs
	TArray<UEdGraph*> AllGraphs;
	AllGraphs.Append(Blueprint->FunctionGraphs);
	AllGraphs.Append(Blueprint->UbergraphPages);
	AllGraphs.Append(Blueprint->DelegateSignatureGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			// Check for function calls
			if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				UFunction* Function = CallNode->GetTargetFunction();
				if (!Function) continue;

				// Only include native/C++ functions
				if (!Function->HasAnyFunctionFlags(FUNC_Native) &&
					!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
				{
					continue;
				}

				FBlueprintCppFunctionUsage Usage;
				Usage.FunctionName = Function->GetName();
				Usage.FunctionClass = Function->GetOwnerClass() ? Function->GetOwnerClass()->GetName() : TEXT("");
				Usage.BlueprintPath = BlueprintPath;
				Usage.NodeGuid = CallNode->NodeGuid.ToString();
				Usage.GraphName = Graph->GetName();
				Usage.bIsBlueprintCallable = Function->HasAnyFunctionFlags(FUNC_BlueprintCallable);
				Usage.bIsBlueprintNativeEvent = IsBlueprintNativeEvent(Function);
				Usage.bIsBlueprintImplementableEvent = IsBlueprintImplementableEvent(Function);
				Usage.bIsImplementation = false;

				Results.Add(Usage);
			}
			// Check for event implementations
			else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				UFunction* Function = EventNode->FindEventSignatureFunction();
				if (!Function) continue;

				if (IsBlueprintNativeEvent(Function) ||
					IsBlueprintImplementableEvent(Function))
				{
					FBlueprintCppFunctionUsage Usage;
					Usage.FunctionName = Function->GetName();
					Usage.FunctionClass = Function->GetOwnerClass() ? Function->GetOwnerClass()->GetName() : TEXT("");
					Usage.BlueprintPath = BlueprintPath;
					Usage.NodeGuid = EventNode->NodeGuid.ToString();
					Usage.GraphName = Graph->GetName();
					Usage.bIsBlueprintCallable = Function->HasAnyFunctionFlags(FUNC_BlueprintCallable);
					Usage.bIsBlueprintNativeEvent = IsBlueprintNativeEvent(Function);
					Usage.bIsBlueprintImplementableEvent = IsBlueprintImplementableEvent(Function);
					Usage.bIsImplementation = true;

					Results.Add(Usage);
				}
			}
		}
	}
#endif

	return Results;
}

TArray<FBlueprintPropertySearchResult> UBlueprintExportReader::FindBlueprintsWithPropertyValue(
	const FString& PropertyName,
	const FString& PropertyValue,
	const FString& ParentClassName,
	const TArray<FString>& SearchPaths)
{
	TArray<FBlueprintPropertySearchResult> Results;

	// Find the parent class if specified
	UClass* ParentClass = nullptr;
	if (!ParentClassName.IsEmpty())
	{
		ParentClass = FindObject<UClass>(ANY_PACKAGE, *ParentClassName);
		if (!ParentClass)
		{
			// Try with common prefixes
			ParentClass = FindObject<UClass>(ANY_PACKAGE, *(TEXT("U") + ParentClassName));
			if (!ParentClass)
			{
				ParentClass = FindObject<UClass>(ANY_PACKAGE, *(TEXT("A") + ParentClassName));
			}
		}
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	for (const FString& SearchPath : SearchPaths)
	{
		TArray<FAssetData> AssetList;
		AssetRegistry.GetAssetsByPath(FName(*SearchPath), AssetList, true);

		for (const FAssetData& AssetData : AssetList)
		{
			// Check if this is a blueprint
			if (!AssetData.AssetClass.ToString().Contains(TEXT("Blueprint")))
			{
				continue;
			}

			// Load the blueprint
			UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
			if (!Blueprint || !Blueprint->GeneratedClass)
			{
				continue;
			}

			UClass* GeneratedClass = Blueprint->GeneratedClass;

			// Check parent class filter
			if (ParentClass && !GeneratedClass->IsChildOf(ParentClass))
			{
				continue;
			}

			// Get the CDO
			UObject* CDO = GeneratedClass->GetDefaultObject();
			if (!CDO)
			{
				continue;
			}

			// Find the property
			FProperty* Property = GeneratedClass->FindPropertyByName(FName(*PropertyName));
			if (!Property)
			{
				continue;
			}

			// Get the property value as string
			FString CurrentValue;
			void* PropertyAddress = Property->ContainerPtrToValuePtr<void>(CDO);

			if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
			{
				bool bValue = BoolProp->GetPropertyValue(PropertyAddress);
				CurrentValue = bValue ? TEXT("true") : TEXT("false");
			}
			else if (FIntProperty* IntProp = CastField<FIntProperty>(Property))
			{
				int32 Value = IntProp->GetPropertyValue(PropertyAddress);
				CurrentValue = FString::FromInt(Value);
			}
			else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
			{
				float Value = FloatProp->GetPropertyValue(PropertyAddress);
				CurrentValue = FString::SanitizeFloat(Value);
			}
			else if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
			{
				CurrentValue = StrProp->GetPropertyValue(PropertyAddress);
			}
			else if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
			{
				CurrentValue = NameProp->GetPropertyValue(PropertyAddress).ToString();
			}
			else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
			{
				UEnum* Enum = EnumProp->GetEnum();
				FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
				int64 Value = UnderlyingProp->GetSignedIntPropertyValue(PropertyAddress);
				CurrentValue = Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(Value);
			}
			else if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
			{
				uint8 Value = ByteProp->GetPropertyValue(PropertyAddress);
				if (ByteProp->Enum)
				{
					CurrentValue = ByteProp->Enum->GetNameStringByValue(Value);
				}
				else
				{
					CurrentValue = FString::FromInt(Value);
				}
			}
			else
			{
				// For other property types, try to export to string
				Property->ExportTextItem(CurrentValue, PropertyAddress, nullptr, nullptr, PPF_None);
			}

			// Check value filter
			if (!PropertyValue.IsEmpty() && !CurrentValue.Equals(PropertyValue, ESearchCase::IgnoreCase))
			{
				continue;
			}

			// Add to results
			FBlueprintPropertySearchResult Result;
			Result.BlueprintPath = Blueprint->GetPathName();
			Result.BlueprintName = Blueprint->GetName();
			Result.ParentClass = Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT("");
			Result.PropertyName = PropertyName;
			Result.PropertyValue = CurrentValue;
			Result.PropertyType = Property->GetCPPType();

			Results.Add(Result);
		}
	}

	return Results;
}

TArray<FBlueprintSearchResult> UBlueprintExportReader::SearchInBlueprints(
	const FString& Query,
	const TArray<FString>& SearchPaths)
{
	TArray<FBlueprintSearchResult> Results;

	if (Query.IsEmpty()) { return Results; }

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	for (const FString& SearchPath : SearchPaths)
	{
		TArray<FAssetData> AssetList;
		AssetRegistry.GetAssetsByPath(FName(*SearchPath), AssetList, true);

		for (const FAssetData& AssetData : AssetList)
		{
			if (!AssetData.AssetClass.ToString().Contains(TEXT("Blueprint")))
			{
				continue;
			}

			UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
			if (!Blueprint) { continue; }

			const FString BPPath = Blueprint->GetPathName();

			// Search variable names.
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				if (Var.VarName.ToString().Contains(Query, ESearchCase::IgnoreCase))
				{
					FBlueprintSearchResult Hit;
					Hit.BlueprintPath = BPPath;
					Hit.MatchField = TEXT("VariableName");
					Hit.MatchValue = Var.VarName.ToString();
					Results.Add(Hit);
				}
			}

			// Gather all graphs.
			TArray<UEdGraph*> AllGraphs;
#if WITH_EDITORONLY_DATA
			AllGraphs.Append(Blueprint->FunctionGraphs);
			AllGraphs.Append(Blueprint->UbergraphPages);
			AllGraphs.Append(Blueprint->MacroGraphs);
			AllGraphs.Append(Blueprint->DelegateSignatureGraphs);
#endif

			// Search function / event graph names.
			for (const UEdGraph* Graph : AllGraphs)
			{
				if (!Graph) { continue; }
				if (Graph->GetName().Contains(Query, ESearchCase::IgnoreCase))
				{
					FBlueprintSearchResult Hit;
					Hit.BlueprintPath = BPPath;
					Hit.GraphName = Graph->GetName();
					Hit.MatchField = TEXT("GraphName");
					Hit.MatchValue = Graph->GetName();
					Results.Add(Hit);
				}
			}

			// Search nodes: titles, comments, pin names, pin defaults.
			for (const UEdGraph* Graph : AllGraphs)
			{
				if (!Graph) { continue; }
				const FString GraphName = Graph->GetName();

				for (const UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node) { continue; }

					const FString NodeGuid = Node->NodeGuid.ToString();
					const FString NodeClassName = Node->GetClass()->GetName();

					// Node title — guard against crash on broken nodes.
					{
						FString Title;
						// GetNodeTitle can crash on broken struct nodes, so wrap carefully.
						if (!IsPinTypeBroken_AnyPin(Node))
						{
							Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
						}
						else
						{
							Title = NodeClassName;
						}
						if (Title.Contains(Query, ESearchCase::IgnoreCase))
						{
							FBlueprintSearchResult Hit;
							Hit.BlueprintPath = BPPath;
							Hit.GraphName = GraphName;
							Hit.NodeGuid = NodeGuid;
							Hit.NodeClass = NodeClassName;
							Hit.MatchField = TEXT("NodeTitle");
							Hit.MatchValue = Title;
							Results.Add(Hit);
						}
					}

					// Node comment.
					if (!Node->NodeComment.IsEmpty() && Node->NodeComment.Contains(Query, ESearchCase::IgnoreCase))
					{
						FBlueprintSearchResult Hit;
						Hit.BlueprintPath = BPPath;
						Hit.GraphName = GraphName;
						Hit.NodeGuid = NodeGuid;
						Hit.NodeClass = NodeClassName;
						Hit.MatchField = TEXT("NodeComment");
						Hit.MatchValue = Node->NodeComment;
						Results.Add(Hit);
					}

					// Pins: names and defaults.
					for (const UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin) { continue; }

						if (Pin->PinName.ToString().Contains(Query, ESearchCase::IgnoreCase))
						{
							FBlueprintSearchResult Hit;
							Hit.BlueprintPath = BPPath;
							Hit.GraphName = GraphName;
							Hit.NodeGuid = NodeGuid;
							Hit.NodeClass = NodeClassName;
							Hit.MatchField = TEXT("PinName");
							Hit.MatchValue = Pin->PinName.ToString();
							Results.Add(Hit);
						}

						if (!Pin->DefaultValue.IsEmpty() && Pin->DefaultValue.Contains(Query, ESearchCase::IgnoreCase))
						{
							FBlueprintSearchResult Hit;
							Hit.BlueprintPath = BPPath;
							Hit.GraphName = GraphName;
							Hit.NodeGuid = NodeGuid;
							Hit.NodeClass = NodeClassName;
							Hit.MatchField = TEXT("PinDefault");
							Hit.MatchValue = Pin->PinName.ToString() + TEXT("=") + Pin->DefaultValue;
							Results.Add(Hit);
						}
					}
				}
			}
		}
	}

	return Results;
}

TArray<FBlueprintReferenceData> UBlueprintExportReader::GetAssetReferencers(const FString& AssetPath, bool bIncludeSoftReferences)
{
	TArray<FBlueprintReferenceData> Referencers;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Convert asset path to package name
	FString PackageName = AssetPath;
	if (PackageName.Contains(TEXT(".")))
	{
		// Strip the asset name portion (e.g., "/Game/Path/Asset.Asset" -> "/Game/Path/Asset")
		PackageName = FPackageName::ObjectPathToPackageName(PackageName);
	}

	// Get all referencers using the new API
	TArray<FAssetDependency> AllReferencers;
	AssetRegistry.GetReferencers(FName(*PackageName), AllReferencers, UE::AssetRegistry::EDependencyCategory::Package);

	for (const FAssetDependency& Ref : AllReferencers)
	{
		bool bIsHard = (Ref.Properties & UE::AssetRegistry::EDependencyProperty::Hard) != UE::AssetRegistry::EDependencyProperty::None;

		// Skip soft references if not requested
		if (!bIsHard && !bIncludeSoftReferences)
		{
			continue;
		}

		FBlueprintReferenceData RefData;
		RefData.ReferencePath = Ref.AssetId.PackageName.ToString();
		RefData.bIsHardReference = bIsHard;
		RefData.ReferenceType = bIsHard ? TEXT("Referencer") : TEXT("SoftReferencer");
		Referencers.Add(RefData);
	}

	return Referencers;
}

FAssetReferenceGraph UBlueprintExportReader::BuildReferenceViewerGraph(
	const FString& RootAssetPath,
	int32 DependencyDepth,
	int32 ReferencerDepth,
	bool bIncludeSoftReferences,
	bool bBlueprintsOnly)
{
	FAssetReferenceGraph Graph;
	Graph.RootAssetPath = RootAssetPath;
	Graph.DependencyDepth = DependencyDepth;
	Graph.ReferencerDepth = ReferencerDepth;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Helper lambda to check if an asset is a Blueprint
	auto IsBlueprint = [&AssetRegistry](const FString& PackagePath) -> bool
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(FName(*PackagePath), Assets);
		for (const FAssetData& Asset : Assets)
		{
			FString ClassName = Asset.AssetClass.ToString();
			if (ClassName.Contains(TEXT("Blueprint")) || ClassName.Contains(TEXT("WidgetBlueprint")))
			{
				return true;
			}
		}
		return false;
	};

	// Helper lambda to get asset info
	auto GetAssetInfo = [&AssetRegistry](const FString& PackagePath) -> TPair<FString, FString>
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(FName(*PackagePath), Assets);
		if (Assets.Num() > 0)
		{
			return TPair<FString, FString>(Assets[0].AssetName.ToString(), Assets[0].AssetClass.ToString());
		}
		// For native classes, extract from path
		if (PackagePath.StartsWith(TEXT("/Script/")))
		{
			FString ClassName = PackagePath;
			ClassName.RemoveFromStart(TEXT("/Script/"));
			int32 DotIndex;
			if (ClassName.FindChar('.', DotIndex))
			{
				ClassName = ClassName.Mid(DotIndex + 1);
			}
			return TPair<FString, FString>(ClassName, TEXT("Class"));
		}
		return TPair<FString, FString>(FPackageName::GetShortName(PackagePath), TEXT("Unknown"));
	};

	// Helper to add or get a node
	auto GetOrCreateNode = [&Graph, &GetAssetInfo](const FString& PackagePath, int32 Depth) -> FAssetReferenceNode&
	{
		if (!Graph.Nodes.Contains(PackagePath))
		{
			FAssetReferenceNode NewNode;
			NewNode.AssetPath = PackagePath;
			NewNode.Depth = Depth;

			TPair<FString, FString> Info = GetAssetInfo(PackagePath);
			NewNode.AssetName = Info.Key;
			NewNode.AssetClass = Info.Value;
			NewNode.bIsBlueprint = Info.Value.Contains(TEXT("Blueprint"));
			NewNode.bIsNativeClass = PackagePath.StartsWith(TEXT("/Script/"));

			Graph.Nodes.Add(PackagePath, NewNode);

			// Update counts
			if (NewNode.bIsBlueprint)
			{
				Graph.BlueprintCount++;
			}
			if (NewNode.bIsNativeClass)
			{
				Graph.NativeClassCount++;
			}
		}
		return Graph.Nodes[PackagePath];
	};

	// Convert root path to package name
	FString RootPackage = RootAssetPath;
	if (RootPackage.Contains(TEXT(".")))
	{
		RootPackage = FPackageName::ObjectPathToPackageName(RootPackage);
	}

	// Create root node
	GetOrCreateNode(RootPackage, 0);

	// BFS for dependencies (positive depth = what root uses)
	if (DependencyDepth > 0)
	{
		TArray<TPair<FString, int32>> ToProcess;
		TSet<FString> Visited;
		ToProcess.Add(TPair<FString, int32>(RootPackage, 0));

		while (ToProcess.Num() > 0)
		{
			TPair<FString, int32> Current = ToProcess[0];
			ToProcess.RemoveAt(0);

			FString CurrentPath = Current.Key;
			int32 CurrentDepth = Current.Value;

			if (Visited.Contains(CurrentPath) || CurrentDepth >= DependencyDepth)
			{
				continue;
			}
			Visited.Add(CurrentPath);

			// Get dependencies using new API
			TArray<FAssetDependency> AllDepsData;
			AssetRegistry.GetDependencies(FName(*CurrentPath), AllDepsData, UE::AssetRegistry::EDependencyCategory::Package);

			FAssetReferenceNode& CurrentNode = GetOrCreateNode(CurrentPath, CurrentDepth);

			for (const FAssetDependency& DepData : AllDepsData)
			{
				bool bIsHard = (DepData.Properties & UE::AssetRegistry::EDependencyProperty::Hard) != UE::AssetRegistry::EDependencyProperty::None;

				// Skip soft references if not requested
				if (!bIsHard && !bIncludeSoftReferences)
				{
					continue;
				}

				FString DepPath = DepData.AssetId.PackageName.ToString();

				// Skip engine/editor content unless it's a script reference
				if ((DepPath.StartsWith(TEXT("/Engine/")) || DepPath.StartsWith(TEXT("/Editor/"))) &&
					!DepPath.StartsWith(TEXT("/Script/")))
				{
					continue;
				}

				// Apply blueprint filter if requested
				if (bBlueprintsOnly && !DepPath.StartsWith(TEXT("/Script/")))
				{
					if (!IsBlueprint(DepPath))
					{
						continue;
					}
				}

				// Create/update the dependency node
				FAssetReferenceNode& DepNode = GetOrCreateNode(DepPath, CurrentDepth + 1);
				DepNode.bIsHardReference = bIsHard;

				// Add to current node's dependencies list
				CurrentNode.Dependencies.AddUnique(DepPath);

				// Add to dependency node's referencers list
				DepNode.Referencers.AddUnique(CurrentPath);

				// Queue for further processing
				if (CurrentDepth + 1 < DependencyDepth)
				{
					ToProcess.Add(TPair<FString, int32>(DepPath, CurrentDepth + 1));
				}

				Graph.TotalDependencies++;
			}
		}
	}

	// BFS for referencers (negative depth = what uses root)
	if (ReferencerDepth > 0)
	{
		TArray<TPair<FString, int32>> ToProcess;
		TSet<FString> Visited;
		ToProcess.Add(TPair<FString, int32>(RootPackage, 0));

		while (ToProcess.Num() > 0)
		{
			TPair<FString, int32> Current = ToProcess[0];
			ToProcess.RemoveAt(0);

			FString CurrentPath = Current.Key;
			int32 CurrentDepth = Current.Value;  // This is positive, but represents steps toward referencers

			if (Visited.Contains(CurrentPath) || CurrentDepth >= ReferencerDepth)
			{
				continue;
			}
			Visited.Add(CurrentPath);

			// Get referencers using new API
			TArray<FAssetDependency> AllRefsData;
			AssetRegistry.GetReferencers(FName(*CurrentPath), AllRefsData, UE::AssetRegistry::EDependencyCategory::Package);

			FAssetReferenceNode& CurrentNode = GetOrCreateNode(CurrentPath, -CurrentDepth);

			for (const FAssetDependency& RefData : AllRefsData)
			{
				bool bIsHard = (RefData.Properties & UE::AssetRegistry::EDependencyProperty::Hard) != UE::AssetRegistry::EDependencyProperty::None;

				// Skip soft references if not requested
				if (!bIsHard && !bIncludeSoftReferences)
				{
					continue;
				}

				FString RefPath = RefData.AssetId.PackageName.ToString();

				// Skip engine/editor content
				if ((RefPath.StartsWith(TEXT("/Engine/")) || RefPath.StartsWith(TEXT("/Editor/"))) &&
					!RefPath.StartsWith(TEXT("/Script/")))
				{
					continue;
				}

				// Apply blueprint filter if requested
				if (bBlueprintsOnly && !RefPath.StartsWith(TEXT("/Script/")))
				{
					if (!IsBlueprint(RefPath))
					{
						continue;
					}
				}

				// Create/update the referencer node (negative depth)
				FAssetReferenceNode& RefNode = GetOrCreateNode(RefPath, -(CurrentDepth + 1));
				RefNode.bIsHardReference = bIsHard;

				// Add to current node's referencers list
				CurrentNode.Referencers.AddUnique(RefPath);

				// Add to referencer node's dependencies list
				RefNode.Dependencies.AddUnique(CurrentPath);

				// Queue for further processing
				if (CurrentDepth + 1 < ReferencerDepth)
				{
					ToProcess.Add(TPair<FString, int32>(RefPath, CurrentDepth + 1));
				}

				Graph.TotalReferencers++;
			}
		}
	}

	return Graph;
}
