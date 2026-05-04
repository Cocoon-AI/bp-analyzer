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
// UMG: WidgetTree introspection. Headers in UMG (UWidget, UPanelWidget, UPanelSlot,
// UWidgetTree) + UMGEditor (UWidgetBlueprint). Both modules added to Build.cs.
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
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
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "Engine/UserDefinedStruct.h"
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

		// Copy the full meta array. cppgen needs ExposeOnSpawn, Category overrides,
		// UIMin/UIMax, EditCondition, BlueprintBaseOnly, and any custom keys to
		// round-trip into the generated C++ UPROPERTY. Keys are serialized as-is
		// (FName -> FString); values stay verbatim so quoted strings survive.
		for (const FBPVariableMetaDataEntry& Entry : Var.MetaDataArray)
		{
			VarData.MetaData.Add(Entry.DataKey.ToString(), Entry.DataValue);
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

	// UMG widget tree (only present on UWidgetBlueprint assets). Walk from
	// WidgetTree->RootWidget and recurse via UPanelWidget::GetSlots(). Property
	// dump is class-aware: common fields (Visibility, ToolTipText) on every
	// widget, Text + Font + color fields on widgets that have them. Values are
	// emitted as UE-text-format strings via ExportTextItem so the edit-side
	// `widget set-property` path can round-trip them via ImportText.
	if (UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Blueprint))
	{
		UWidgetTree* Tree = WidgetBP->WidgetTree;
		if (Tree && Tree->RootWidget)
		{
			TFunction<FBlueprintWidgetTreeNode(UWidget*, const FString&)> Walk;
			Walk = [&Walk](UWidget* W, const FString& ParentSlotClass) -> FBlueprintWidgetTreeNode
			{
				FBlueprintWidgetTreeNode Node;
				if (!W) { return Node; }

				Node.Name = W->GetName();
				Node.Class = W->GetClass()->GetName();
				Node.ParentSlotClass = ParentSlotClass;

				// Property dump. Try each candidate by name; FindPropertyByName
				// returns null when the class doesn't have it, so we naturally
				// only emit applicable fields. Order is deterministic so diffs
				// across exports are stable.
				static const TCHAR* CandidateProps[] = {
					TEXT("Visibility"),
					TEXT("ToolTipText"),
					TEXT("Text"),
					TEXT("Font"),
					TEXT("ColorAndOpacity"),
					TEXT("ShadowColorAndOpacity"),
					TEXT("ShadowOffset"),
					TEXT("Justification"),
					TEXT("AutoWrapText"),
					TEXT("WrapTextAt"),
					TEXT("Margin"),
					TEXT("MinDesiredWidth"),
				};
				for (const TCHAR* PropName : CandidateProps)
				{
					FProperty* Prop = W->GetClass()->FindPropertyByName(FName(PropName));
					if (!Prop) { continue; }
					const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(W);
					FString Out;
					Prop->ExportTextItem(Out, ValuePtr, /*Defaults=*/nullptr, /*Parent=*/W, PPF_None);
					Node.Properties.Add(PropName, Out);
				}

				// Recurse on panels. UPanelWidget exposes GetSlots() returning
				// UPanelSlot*; each slot's Content is the child widget. Non-
				// panel widgets stop here (Children stays empty).
				if (UPanelWidget* Panel = Cast<UPanelWidget>(W))
				{
					for (UPanelSlot* PSlot : Panel->GetSlots())
					{
						if (!PSlot || !PSlot->Content) { continue; }
						const FString SlotClassName = PSlot->GetClass()->GetName();
						Node.Children.Add(Walk(PSlot->Content, SlotClassName));
					}
				}

				return Node;
			};

			ExportData.WidgetTree.Add(Walk(Tree->RootWidget, FString()));
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

					// Resolve self-scope var references against the BP's own generated class.
					// Passing nullptr here caused inherited native UPROPERTYs to report as
					// VariableClass="" — which hid C++-owned variable accesses from cpp-audit
					// and from callers filtering by OwningClass. Prefer SkeletonGeneratedClass
					// because that's the editor-time class the K2 nodes resolve against.
					UClass* SelfScope = Blueprint->SkeletonGeneratedClass
						? (UClass*)Blueprint->SkeletonGeneratedClass
						: (UClass*)Blueprint->GeneratedClass;

					if (UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(Node))
					{
						if (!bIncludeSet) continue;
						AccessKind = TEXT("set");
						VarName = SetNode->GetVarName();
						OwningClass = SetNode->VariableReference.GetMemberParentClass(SelfScope);
					}
					else if (UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(Node))
					{
						if (!bIncludeGet) continue;
						AccessKind = TEXT("get");
						VarName = GetNode->GetVarName();
						OwningClass = GetNode->VariableReference.GetMemberParentClass(SelfScope);
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

// ---- cpp-audit helpers ---------------------------------------------------
//
// Native-symbol detection for FBlueprintCppAudit. A class/struct is "native"
// when it exists purely in C++ (ClassGeneratedBy / StructGeneratedBy is null).
// SKEL_ and REINST_ classes appear during editor load as duplicates of native
// classes with suffixed names; report them against their owner Blueprint's
// generated class instead of leaking the reinstancer name.

namespace CppAudit
{
	static bool IsNativeClass(const UClass* Cls)
	{
		return Cls && Cls->ClassGeneratedBy == nullptr;
	}

	static bool IsNativeStruct(const UScriptStruct* S)
	{
		return S && Cast<UUserDefinedStruct>(S) == nullptr;
	}

	// Strip leading U/A/F so exported names match the C++ source names callers grep for.
	static FString StripPrefix(const FString& ClassName)
	{
		if (ClassName.Len() > 1)
		{
			const TCHAR C = ClassName[0];
			if ((C == TEXT('U') || C == TEXT('A') || C == TEXT('F')) && FChar::IsUpper(ClassName[1]))
			{
				return ClassName.Mid(1);
			}
		}
		return ClassName;
	}

	static FString NativeClassName(const UClass* Cls)
	{
		return Cls ? Cls->GetName() : FString();
	}

	// Track one BP's distinct references + push every new one into the reverse index.
	struct FRefAggregator
	{
		TArray<FBlueprintCppSymbolRef> BpRefs;
		TSet<FString> BpSeen;
		TMap<FString, TSet<FString>>& ReverseIndex; // key = Kind|Owner|Name, value = set of caller paths
		TMap<FString, FBlueprintCppSymbolRef>& SymbolMeta; // key -> canonical symbol metadata
		FString BlueprintPath;

		FRefAggregator(
			TMap<FString, TSet<FString>>& InReverse,
			TMap<FString, FBlueprintCppSymbolRef>& InMeta,
			const FString& InBpPath)
			: ReverseIndex(InReverse), SymbolMeta(InMeta), BlueprintPath(InBpPath) {}

		void Record(const FString& Name, const FString& Owner, const FString& Kind)
		{
			if (Name.IsEmpty() && Owner.IsEmpty()) return;
			const FString Key = Kind + TEXT("|") + Owner + TEXT("|") + Name;
			if (!BpSeen.Contains(Key))
			{
				BpSeen.Add(Key);
				FBlueprintCppSymbolRef Ref;
				Ref.Name = Name;
				Ref.Owner = Owner;
				Ref.Kind = Kind;
				BpRefs.Add(Ref);
			}
			// Reverse index accumulates across BPs
			ReverseIndex.FindOrAdd(Key).Add(BlueprintPath);
			if (!SymbolMeta.Contains(Key))
			{
				FBlueprintCppSymbolRef Meta;
				Meta.Name = Name;
				Meta.Owner = Owner;
				Meta.Kind = Kind;
				SymbolMeta.Add(Key, Meta);
			}
		}
	};

	// Emit UClass / USTRUCT refs for an object referenced by a pin type (e.g. a BP
	// member variable whose type is a native class/struct).
	static void RecordPinTypeObject(FRefAggregator& Agg, const UObject* SubObject)
	{
		if (!SubObject) return;
		if (const UClass* AsClass = Cast<UClass>(SubObject))
		{
			if (IsNativeClass(AsClass))
			{
				Agg.Record(NativeClassName(AsClass), FString(), TEXT("UClass"));
			}
		}
		else if (const UScriptStruct* AsStruct = Cast<UScriptStruct>(SubObject))
		{
			if (IsNativeStruct(AsStruct))
			{
				Agg.Record(AsStruct->GetName(), FString(), TEXT("USTRUCT"));
			}
		}
	}
}

FBlueprintCppAudit UBlueprintExportReader::BuildCppReferenceAudit(const TArray<FString>& SearchPaths)
{
	FBlueprintCppAudit Audit;
	Audit.SearchPaths = SearchPaths;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TMap<FString, TSet<FString>> ReverseIndex;
	TMap<FString, FBlueprintCppSymbolRef> SymbolMeta;
	TArray<FBlueprintCppAuditBp> BpEntries;

	for (const FString& SearchPath : SearchPaths)
	{
		TArray<FAssetData> AssetList;
		AssetRegistry.GetAssetsByPath(FName(*SearchPath), AssetList, true);

		for (const FAssetData& AssetData : AssetList)
		{
			// Pre-filter on AssetClass BEFORE GetAsset() to avoid loading every
			// .uasset in the search path. Blueprint/WidgetBlueprint/AnimBlueprint
			// all inherit from UBlueprint.
			const FString AssetClassName = AssetData.AssetClass.ToString();
			if (!AssetClassName.Contains(TEXT("Blueprint")))
			{
				continue;
			}

			UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
			if (!Blueprint || !Blueprint->GeneratedClass)
			{
				continue;
			}

			// Skip REINST_ / SKEL_ / TRASHCLASS_ synthetic classes surfaced during
			// editor hot-reload. These aren't canonical BPs and would corrupt
			// the reverse index if we counted them.
			if (Blueprint->GeneratedClass->HasAnyClassFlags(CLASS_NewerVersionExists))
			{
				continue;
			}

			const FString BpPath = Blueprint->GetPathName();
			FBlueprintCppAuditBp BpEntry;
			BpEntry.BlueprintPath = BpPath;

			CppAudit::FRefAggregator Agg(ReverseIndex, SymbolMeta, BpPath);

			// ---- 1. Parent class ---------------------------------------------
			if (UClass* Parent = Blueprint->ParentClass)
			{
				if (CppAudit::IsNativeClass(Parent))
				{
					BpEntry.ParentCppClass = CppAudit::NativeClassName(Parent);
					Agg.Record(CppAudit::NativeClassName(Parent), FString(), TEXT("ParentClass"));
				}
			}

			// ---- 2. Native-typed member variables ----------------------------
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				if (UObject* SubObj = Var.VarType.PinSubCategoryObject.Get())
				{
					CppAudit::RecordPinTypeObject(Agg, SubObj);
				}
			}

			// ---- 3. Graph walk ------------------------------------------------
			UClass* SelfScope = Blueprint->SkeletonGeneratedClass
				? (UClass*)Blueprint->SkeletonGeneratedClass
				: (UClass*)Blueprint->GeneratedClass;

#if WITH_EDITORONLY_DATA
			TArray<UEdGraph*> AllGraphs;
			AllGraphs.Append(Blueprint->FunctionGraphs);
			AllGraphs.Append(Blueprint->UbergraphPages);
			AllGraphs.Append(Blueprint->MacroGraphs);
			AllGraphs.Append(Blueprint->DelegateSignatureGraphs);

			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph) continue;

				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node) continue;

					// --- K2Node_CallFunction: native UFUNCTION call ---
					if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
					{
						UFunction* Function = CallNode->GetTargetFunction();
						if (!Function) continue;
						UClass* OwnerClass = Function->GetOwnerClass();
						if (!CppAudit::IsNativeClass(OwnerClass)) continue;
						Agg.Record(
							Function->GetName(),
							CppAudit::NativeClassName(OwnerClass),
							TEXT("UFUNCTION"));
						continue;
					}

					// --- K2Node_VariableGet/Set: native UPROPERTY access ---
					if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
					{
						UClass* OwnerClass = VarNode->VariableReference.GetMemberParentClass(SelfScope);
						if (!CppAudit::IsNativeClass(OwnerClass)) continue;
						const FName VarName = VarNode->GetVarName();
						if (VarName.IsNone()) continue;
						Agg.Record(
							VarName.ToString(),
							CppAudit::NativeClassName(OwnerClass),
							TEXT("UPROPERTY"));
						continue;
					}

					// --- K2Node_BaseMCDelegate: BlueprintAssignable delegate bind/call ---
					if (UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(Node))
					{
						UClass* OwnerClass = DelegateNode->DelegateReference.GetMemberParentClass(SelfScope);
						if (!CppAudit::IsNativeClass(OwnerClass)) continue;
						const FName DelegateName = DelegateNode->DelegateReference.GetMemberName();
						if (DelegateName.IsNone()) continue;
						Agg.Record(
							DelegateName.ToString(),
							CppAudit::NativeClassName(OwnerClass),
							TEXT("BlueprintAssignable"));
						continue;
					}

					// --- K2Node_DynamicCast: cast to native UClass ---
					if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
					{
						UClass* Target = CastNode->TargetType;
						if (!CppAudit::IsNativeClass(Target)) continue;
						Agg.Record(
							CppAudit::NativeClassName(Target),
							FString(),
							TEXT("UClass"));
						continue;
					}

					// --- K2Node_MakeStruct / BreakStruct: native USTRUCT reference ---
					if (UK2Node_MakeStruct* MakeNode = Cast<UK2Node_MakeStruct>(Node))
					{
						if (CppAudit::IsNativeStruct(MakeNode->StructType))
						{
							Agg.Record(
								MakeNode->StructType->GetName(),
								FString(),
								TEXT("USTRUCT"));
						}
						continue;
					}
					if (UK2Node_BreakStruct* BreakNode = Cast<UK2Node_BreakStruct>(Node))
					{
						if (CppAudit::IsNativeStruct(BreakNode->StructType))
						{
							Agg.Record(
								BreakNode->StructType->GetName(),
								FString(),
								TEXT("USTRUCT"));
						}
						continue;
					}

					// --- K2Node_Event: override of native BlueprintImplementable/NativeEvent ---
					if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
					{
						UFunction* Sig = EventNode->FindEventSignatureFunction();
						if (!Sig) continue;
						UClass* SigOwner = Sig->GetOwnerClass();
						if (!CppAudit::IsNativeClass(SigOwner)) continue;
						const TCHAR* Kind = IsBlueprintNativeEvent(Sig)
							? TEXT("BlueprintNativeEvent")
							: IsBlueprintImplementableEvent(Sig)
								? TEXT("BlueprintImplementableEvent")
								: TEXT("UFUNCTION");
						Agg.Record(
							Sig->GetName(),
							CppAudit::NativeClassName(SigOwner),
							Kind);
						continue;
					}
				}
			}
#endif // WITH_EDITORONLY_DATA

			BpEntry.References = MoveTemp(Agg.BpRefs);
			BpEntries.Add(MoveTemp(BpEntry));
		}
	}

	Audit.BlueprintCount = BpEntries.Num();

	// Flatten reverse index into sorted Symbols array.
	Audit.Symbols.Reserve(ReverseIndex.Num());
	for (TPair<FString, TSet<FString>>& Pair : ReverseIndex)
	{
		FBlueprintCppAuditSymbol Sym;
		const FBlueprintCppSymbolRef& Meta = SymbolMeta[Pair.Key];
		Sym.Name = Meta.Name;
		Sym.Owner = Meta.Owner;
		Sym.Kind = Meta.Kind;
		Sym.Callers = Pair.Value.Array();
		Sym.Callers.Sort();
		Audit.Symbols.Add(MoveTemp(Sym));
	}
	Audit.Symbols.Sort([](const FBlueprintCppAuditSymbol& A, const FBlueprintCppAuditSymbol& B)
	{
		if (A.Kind != B.Kind) return A.Kind < B.Kind;
		if (A.Owner != B.Owner) return A.Owner < B.Owner;
		return A.Name < B.Name;
	});

	// Sort the forward index by BlueprintPath for determinism.
	BpEntries.Sort([](const FBlueprintCppAuditBp& A, const FBlueprintCppAuditBp& B)
	{
		return A.BlueprintPath < B.BlueprintPath;
	});
	Audit.Blueprints = MoveTemp(BpEntries);

	return Audit;
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

			// Ensure the current node exists. DO NOT hold an FAssetReferenceNode&
			// across subsequent GetOrCreateNode calls — those calls may Add() into
			// Graph.Nodes and trigger a TMap rehash, which invalidates any refs we
			// already obtained. Stale-ref writes then corrupt another entry's
			// TArray<FString> internals, and a later AddUnique crashes in FString
			// compare with a 0x...0049 address pattern. Fetch fresh every time.
			GetOrCreateNode(CurrentPath, CurrentDepth);

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

				// Create the dependency node first. After this, any previously-held
				// ref to another Graph.Nodes entry may be stale, so we re-fetch
				// below via the TMap for every write.
				GetOrCreateNode(DepPath, CurrentDepth + 1);
				Graph.Nodes[DepPath].bIsHardReference = bIsHard;

				// Add to current node's dependencies list (fresh lookup).
				Graph.Nodes[CurrentPath].Dependencies.AddUnique(DepPath);

				// Add to dependency node's referencers list (fresh lookup).
				Graph.Nodes[DepPath].Referencers.AddUnique(CurrentPath);

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

			// Same ref-stability rule as the dep loop above — don't hold an
			// FAssetReferenceNode& across a GetOrCreateNode() that may rehash.
			GetOrCreateNode(CurrentPath, -CurrentDepth);

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

				// Create the referencer node (negative depth). Fresh TMap lookups
				// for each write below — no stale refs across GetOrCreateNode.
				GetOrCreateNode(RefPath, -(CurrentDepth + 1));
				Graph.Nodes[RefPath].bIsHardReference = bIsHard;

				Graph.Nodes[CurrentPath].Referencers.AddUnique(RefPath);
				Graph.Nodes[RefPath].Dependencies.AddUnique(CurrentPath);

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
