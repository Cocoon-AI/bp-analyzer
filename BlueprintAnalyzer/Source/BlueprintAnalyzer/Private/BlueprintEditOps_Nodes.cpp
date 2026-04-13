// BlueprintEditOps_Nodes.cpp
// Phase D edit operations: generic node manipulation (remove, move, add_generic)
// and pin wiring (connect/disconnect/set_default/break_all). Curated high-level
// node builders (add_branch, add_function_call, etc.) live in
// BlueprintEditOps_NodeBuilders.cpp.

#include "BlueprintEditOps.h"
#include "BlueprintEditHelpers.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_SetFieldsInStruct.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

//------------------------------------------------------------------------------
// Local helpers
//------------------------------------------------------------------------------

namespace
{
	using FBlueprintEditHelpers::RequireString;
	using FBlueprintEditHelpers::ResolveBPAndGraph;
	using FBlueprintEditHelpers::BPPath;
	using FBPGraphContext = FBlueprintEditHelpers::FBPGraphContext;

	// Parse a GUID from the params.
	static bool ParseGuidField(const TSharedPtr<FJsonObject>& Params, const FString& Field, FGuid& OutGuid, TSharedPtr<FJsonObject>& OutError)
	{
		FString GuidStr;
		if (!RequireString(Params, Field, GuidStr, OutError)) { return false; }
		if (!FGuid::Parse(GuidStr, OutGuid))
		{
			OutError = FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Invalid GUID for %s: %s"), *Field, *GuidStr));
			return false;
		}
		return true;
	}

	// Look up a node by GUID field, returning a user-friendly error on failure.
	static UEdGraphNode* ResolveNode(const TSharedPtr<FJsonObject>& Params, const FString& Field, UEdGraph* Graph, TSharedPtr<FJsonObject>& OutError)
	{
		FGuid Guid;
		if (!ParseGuidField(Params, Field, Guid, OutError)) { return nullptr; }
		UEdGraphNode* Node = FBlueprintEditHelpers::FindNodeByGuid(Graph, Guid);
		if (!Node)
		{
			OutError = FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Node not found in graph: %s"), *Guid.ToString()));
		}
		return Node;
	}
}

//------------------------------------------------------------------------------
// edit.node.remove
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeRemove(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	UEdGraphNode* Node = ResolveNode(Params, TEXT("node_guid"), Ctx.Graph, Err);
	if (!Node) { return Err; }

	FBlueprintEditorUtils::RemoveNode(Ctx.Blueprint, Node, /*bDontRecompile*/true);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Response->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
	return Response;
}

//------------------------------------------------------------------------------
// edit.node.remove_broken
//------------------------------------------------------------------------------

// Returns true if a pin's type references a struct/object/class/enum whose
// backing UObject no longer exists.
static bool HasBrokenPinType(const UEdGraphPin* Pin)
{
	if (!Pin) { return false; }
	static const TSet<FName> NeedsSubObject = {
		UEdGraphSchema_K2::PC_Struct,
		UEdGraphSchema_K2::PC_Object,
		UEdGraphSchema_K2::PC_Class,
		UEdGraphSchema_K2::PC_SoftObject,
		UEdGraphSchema_K2::PC_SoftClass,
		UEdGraphSchema_K2::PC_Interface,
		UEdGraphSchema_K2::PC_Enum,
	};
	return NeedsSubObject.Contains(Pin->PinType.PinCategory) && !Pin->PinType.PinSubCategoryObject.IsValid();
}

static bool IsNodeBroken(const UEdGraphNode* Node)
{
	if (!Node) { return false; }

	// Check node-level StructType on struct manipulation nodes.
	// These store the struct as a member, not (only) on pins.
	if (const UK2Node_BreakStruct* BreakNode = Cast<UK2Node_BreakStruct>(Node))
	{
		if (!BreakNode->StructType) { return true; }
	}
	else if (const UK2Node_MakeStruct* MakeNode = Cast<UK2Node_MakeStruct>(Node))
	{
		if (!MakeNode->StructType) { return true; }
	}
	else if (const UK2Node_SetFieldsInStruct* SetFieldsNode = Cast<UK2Node_SetFieldsInStruct>(Node))
	{
		if (!SetFieldsNode->StructType) { return true; }
	}

	// Check async action nodes — proxy class null means the latent action class
	// was deleted. Members are protected in UE4.27 with no getters, so use reflection.
	if (const UK2Node_BaseAsyncTask* AsyncNode = Cast<UK2Node_BaseAsyncTask>(Node))
	{
		FObjectProperty* Prop = CastField<FObjectProperty>(
			UK2Node_BaseAsyncTask::StaticClass()->FindPropertyByName(TEXT("ProxyFactoryClass")));
		UClass* FactoryClass = Prop ? Cast<UClass>(Prop->GetObjectPropertyValue_InContainer(AsyncNode)) : nullptr;
		if (!FactoryClass) { return true; }
	}

	// Check pin-level type references.
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (HasBrokenPinType(Pin)) { return true; }
	}
	return false;
}

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeRemoveBroken(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	// Gather all graphs: uber, function, macro, delegate signatures.
	TArray<UEdGraph*> AllGraphs;
	AllGraphs.Append(Blueprint->UbergraphPages);
	AllGraphs.Append(Blueprint->FunctionGraphs);
	AllGraphs.Append(Blueprint->MacroGraphs);
	AllGraphs.Append(Blueprint->DelegateSignatureGraphs);

	// Collect broken nodes across all graphs.
	TArray<UEdGraphNode*> BrokenNodes;
	TArray<TSharedPtr<FJsonValue>> RemovedArray;

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) { continue; }
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) { continue; }
			if (IsNodeBroken(Node))
			{
				BrokenNodes.Add(Node);

				TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject);
				Entry->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
				Entry->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
				Entry->SetStringField(TEXT("graph"), Graph->GetName());

				// Safely get title — broken nodes may crash in GetNodeTitle.
				// Just use the class name; GetNodeTitle can dereference the null struct.
				Entry->SetStringField(TEXT("node_title"), Node->GetClass()->GetName());

				// Flag if the node itself has a null StructType (BreakStruct/MakeStruct/SetFields).
				if ((Cast<UK2Node_BreakStruct>(Node) && !Cast<UK2Node_BreakStruct>(Node)->StructType) ||
					(Cast<UK2Node_MakeStruct>(Node) && !Cast<UK2Node_MakeStruct>(Node)->StructType) ||
					(Cast<UK2Node_SetFieldsInStruct>(Node) && !Cast<UK2Node_SetFieldsInStruct>(Node)->StructType))
				{
					Entry->SetBoolField(TEXT("null_struct_type"), true);
				}

				if (Cast<UK2Node_BaseAsyncTask>(Node))
				{
					// If we got here and it's an async node, the proxy class is null.
					{
						Entry->SetBoolField(TEXT("null_proxy_class"), true);
					}
				}

				// List the broken pins.
				TArray<TSharedPtr<FJsonValue>> BrokenPins;
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (HasBrokenPinType(Pin))
					{
						TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject);
						PinObj->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
						PinObj->SetStringField(TEXT("pin_category"), Pin->PinType.PinCategory.ToString());
						BrokenPins.Add(MakeShareable(new FJsonValueObject(PinObj)));
					}
				}
				Entry->SetArrayField(TEXT("broken_pins"), BrokenPins);

				RemovedArray.Add(MakeShareable(new FJsonValueObject(Entry)));
			}
		}
	}

	// Remove the nodes (unless dry run).
	if (!bDryRun)
	{
		for (UEdGraphNode* Node : BrokenNodes)
		{
			FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile*/true);
		}
		if (BrokenNodes.Num() > 0)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
	}

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetArrayField(bDryRun ? TEXT("broken_nodes") : TEXT("removed_nodes"), RemovedArray);
	Response->SetNumberField(TEXT("count"), RemovedArray.Num());
	if (bDryRun) { Response->SetBoolField(TEXT("dry_run"), true); }
	return Response;
}

//------------------------------------------------------------------------------
// edit.node.move
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeMove(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	UEdGraphNode* Node = ResolveNode(Params, TEXT("node_guid"), Ctx.Graph, Err);
	if (!Node) { return Err; }

	double X = 0, Y = 0;
	if (!Params->TryGetNumberField(TEXT("x"), X) || !Params->TryGetNumberField(TEXT("y"), Y))
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Missing required params: x, y"));
	}

	Node->NodePosX = (int32)X;
	Node->NodePosY = (int32)Y;
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Response->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
	Response->SetNumberField(TEXT("x"), Node->NodePosX);
	Response->SetNumberField(TEXT("y"), Node->NodePosY);
	return Response;
}

//------------------------------------------------------------------------------
// edit.node.add_generic
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeAddGeneric(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	FString NodeClassPath;
	if (!RequireString(Params, TEXT("node_class"), NodeClassPath, Err)) { return Err; }

	UClass* NodeClass = LoadObject<UClass>(nullptr, *NodeClassPath);
	if (!NodeClass || !NodeClass->IsChildOf(UK2Node::StaticClass()))
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("'%s' is not a UK2Node subclass"), *NodeClassPath));
	}

	UK2Node* NewNode = NewObject<UK2Node>(Ctx.Graph, NodeClass);
	NewNode->CreateNewGuid();
	NewNode->PostPlacedNewNode();
	NewNode->SetFlags(RF_Transactional);
	NewNode->AllocateDefaultPins();

	double X = 0, Y = 0;
	Params->TryGetNumberField(TEXT("x"), X);
	Params->TryGetNumberField(TEXT("y"), Y);
	NewNode->NodePosX = (int32)X;
	NewNode->NodePosY = (int32)Y;

	Ctx.Graph->AddNode(NewNode, /*bFromUI*/false, /*bSelectNewNode*/false);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Response->SetObjectField(TEXT("node"), FBlueprintEditHelpers::NodeToEditResponse(NewNode));
	return Response;
}

//------------------------------------------------------------------------------
// edit.pin.set_default
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::PinSetDefault(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	UEdGraphNode* Node = ResolveNode(Params, TEXT("node_guid"), Ctx.Graph, Err);
	if (!Node) { return Err; }

	FString PinName, Value;
	if (!RequireString(Params, TEXT("pin_name"), PinName, Err)) { return Err; }
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Missing required param: value"));
	}

	UEdGraphPin* Pin = FBlueprintEditHelpers::FindPinByName(Node, PinName, EGPD_Input);
	if (!Pin)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Input pin not found: %s"), *PinName));
	}

	if (const UEdGraphSchema* Schema = Ctx.Graph->GetSchema())
	{
		Schema->TrySetDefaultValue(*Pin, Value);
	}
	else
	{
		Pin->DefaultValue = Value;
	}
	Node->PinDefaultValueChanged(Pin);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Response->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
	Response->SetStringField(TEXT("pin_name"), PinName);
	Response->SetStringField(TEXT("value"), Value);
	return Response;
}

//------------------------------------------------------------------------------
// edit.pin.connect
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::PinConnect(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	UEdGraphNode* FromNode = ResolveNode(Params, TEXT("from_node"), Ctx.Graph, Err);
	if (!FromNode) { return Err; }
	UEdGraphNode* ToNode = ResolveNode(Params, TEXT("to_node"), Ctx.Graph, Err);
	if (!ToNode) { return Err; }

	FString FromPinName, ToPinName;
	if (!RequireString(Params, TEXT("from_pin"), FromPinName, Err)) { return Err; }
	if (!RequireString(Params, TEXT("to_pin"), ToPinName, Err)) { return Err; }

	UEdGraphPin* FromPin = FBlueprintEditHelpers::FindPinByName(FromNode, FromPinName, EGPD_Output);
	if (!FromPin)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Output pin not found: %s"), *FromPinName));
	}
	UEdGraphPin* ToPin = FBlueprintEditHelpers::FindPinByName(ToNode, ToPinName, EGPD_Input);
	if (!ToPin)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Input pin not found: %s"), *ToPinName));
	}

	const UEdGraphSchema* Schema = Ctx.Graph->GetSchema();
	if (!Schema)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Graph has no schema"));
	}

	const bool bConnected = Schema->TryCreateConnection(FromPin, ToPin);
	if (!bConnected)
	{
		// TryCreateConnection returns false if pins are incompatible. Surface the
		// schema's diagnostic via CanCreateConnection so agents see *why*.
		const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(
			TEXT("TryCreateConnection failed: %s"), *Response.Message.ToString()));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Resp = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Resp->SetStringField(TEXT("from_node"), FromNode->NodeGuid.ToString());
	Resp->SetStringField(TEXT("from_pin"), FromPinName);
	Resp->SetStringField(TEXT("to_node"), ToNode->NodeGuid.ToString());
	Resp->SetStringField(TEXT("to_pin"), ToPinName);
	return Resp;
}

//------------------------------------------------------------------------------
// edit.pin.disconnect
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::PinDisconnect(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	UEdGraphNode* FromNode = ResolveNode(Params, TEXT("from_node"), Ctx.Graph, Err);
	if (!FromNode) { return Err; }
	UEdGraphNode* ToNode = ResolveNode(Params, TEXT("to_node"), Ctx.Graph, Err);
	if (!ToNode) { return Err; }

	FString FromPinName, ToPinName;
	if (!RequireString(Params, TEXT("from_pin"), FromPinName, Err)) { return Err; }
	if (!RequireString(Params, TEXT("to_pin"), ToPinName, Err)) { return Err; }

	UEdGraphPin* FromPin = FBlueprintEditHelpers::FindPinByName(FromNode, FromPinName, EGPD_Output);
	UEdGraphPin* ToPin = FBlueprintEditHelpers::FindPinByName(ToNode, ToPinName, EGPD_Input);
	if (!FromPin || !ToPin)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Pin(s) not found"));
	}

	const UEdGraphSchema* Schema = Ctx.Graph->GetSchema();
	if (Schema)
	{
		Schema->BreakSinglePinLink(FromPin, ToPin);
	}
	else
	{
		FromPin->BreakLinkTo(ToPin);
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Resp = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Resp->SetStringField(TEXT("from_node"), FromNode->NodeGuid.ToString());
	Resp->SetStringField(TEXT("from_pin"), FromPinName);
	Resp->SetStringField(TEXT("to_node"), ToNode->NodeGuid.ToString());
	Resp->SetStringField(TEXT("to_pin"), ToPinName);
	return Resp;
}

//------------------------------------------------------------------------------
// edit.pin.break_all_links
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::PinBreakAllLinks(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	UEdGraphNode* Node = ResolveNode(Params, TEXT("node_guid"), Ctx.Graph, Err);
	if (!Node) { return Err; }

	FString PinName;
	if (!RequireString(Params, TEXT("pin_name"), PinName, Err)) { return Err; }

	// Try both directions since we don't know which the caller means.
	UEdGraphPin* Pin = FBlueprintEditHelpers::FindPinByName(Node, PinName, EGPD_Input);
	if (!Pin) { Pin = FBlueprintEditHelpers::FindPinByName(Node, PinName, EGPD_Output); }
	if (!Pin)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Pin not found: %s"), *PinName));
	}

	const UEdGraphSchema* Schema = Ctx.Graph->GetSchema();
	if (Schema)
	{
		Schema->BreakPinLinks(*Pin, /*bSendsNodeNotification*/true);
	}
	else
	{
		Pin->BreakAllPinLinks();
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Resp = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Resp->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
	Resp->SetStringField(TEXT("pin_name"), PinName);
	return Resp;
}
