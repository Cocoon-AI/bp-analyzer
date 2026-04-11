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
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"

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
