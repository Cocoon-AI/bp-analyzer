// BlueprintEditOps_NodeBuilders.cpp
// Phase D curated high-level K2 node builders. Each op creates one specific
// K2Node subclass, configures it, places it in the graph, and returns the
// node's GUID + pin layout so the agent can immediately wire it up.

#include "BlueprintEditOps.h"
#include "BlueprintEditHelpers.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_Literal.h"

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

	// Extract optional position; defaults to (0, 0).
	static void ReadPosition(const TSharedPtr<FJsonObject>& Params, int32& OutX, int32& OutY)
	{
		double Val = 0;
		OutX = 0; OutY = 0;
		if (Params->TryGetNumberField(TEXT("x"), Val)) { OutX = (int32)Val; }
		if (Params->TryGetNumberField(TEXT("y"), Val)) { OutY = (int32)Val; }
	}

	// Shared node-placement helper: set guid/flags/pins/position and add to graph.
	template <typename TNode>
	static TNode* FinalizeNodePlacement(UEdGraph* Graph, TNode* Node, int32 X, int32 Y)
	{
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->SetFlags(RF_Transactional);
		Node->AllocateDefaultPins();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Graph->AddNode(Node, /*bFromUI*/false, /*bSelectNewNode*/false);
		return Node;
	}

	// Resolve a class by short name or full path (with common prefix fallbacks).
	static UClass* ResolveClassFallback(const FString& Name)
	{
		if (Name.IsEmpty()) { return nullptr; }
		if (Name.StartsWith(TEXT("/")))
		{
			return LoadObject<UClass>(nullptr, *Name);
		}
		if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *Name))
		{
			return Found;
		}
		for (const TCHAR* Prefix : { TEXT("U"), TEXT("A") })
		{
			if (!Name.StartsWith(Prefix))
			{
				const FString Prefixed = FString(Prefix) + Name;
				if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *Prefixed))
				{
					return Found;
				}
			}
		}
		return nullptr;
	}

	static UScriptStruct* ResolveStructFallback(const FString& Name)
	{
		if (Name.IsEmpty()) { return nullptr; }
		if (Name.StartsWith(TEXT("/")))
		{
			return LoadObject<UScriptStruct>(nullptr, *Name);
		}
		if (UScriptStruct* Found = FindObject<UScriptStruct>(ANY_PACKAGE, *Name))
		{
			return Found;
		}
		if (!Name.StartsWith(TEXT("F")))
		{
			const FString Prefixed = FString(TEXT("F")) + Name;
			if (UScriptStruct* Found = FindObject<UScriptStruct>(ANY_PACKAGE, *Prefixed))
			{
				return Found;
			}
		}
		return nullptr;
	}
}

//------------------------------------------------------------------------------
// edit.node.add_variable_get
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeAddVariableGet(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	FString VarName;
	if (!RequireString(Params, TEXT("variable"), VarName, Err)) { return Err; }

	int32 X = 0, Y = 0;
	ReadPosition(Params, X, Y);

	UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Ctx.Graph);
	Node->VariableReference.SetSelfMember(FName(*VarName));
	FinalizeNodePlacement(Ctx.Graph, Node, X, Y);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Response->SetObjectField(TEXT("node"), FBlueprintEditHelpers::NodeToEditResponse(Node));
	return Response;
}

//------------------------------------------------------------------------------
// edit.node.add_variable_set
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeAddVariableSet(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	FString VarName;
	if (!RequireString(Params, TEXT("variable"), VarName, Err)) { return Err; }

	int32 X = 0, Y = 0;
	ReadPosition(Params, X, Y);

	UK2Node_VariableSet* Node = NewObject<UK2Node_VariableSet>(Ctx.Graph);
	Node->VariableReference.SetSelfMember(FName(*VarName));
	FinalizeNodePlacement(Ctx.Graph, Node, X, Y);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Response->SetObjectField(TEXT("node"), FBlueprintEditHelpers::NodeToEditResponse(Node));
	return Response;
}

//------------------------------------------------------------------------------
// edit.node.add_function_call
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeAddFunctionCall(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	FString FunctionName;
	if (!RequireString(Params, TEXT("function"), FunctionName, Err)) { return Err; }

	FString ClassName;
	Params->TryGetStringField(TEXT("class"), ClassName);

	UClass* OwnerClass = nullptr;
	if (!ClassName.IsEmpty())
	{
		OwnerClass = ResolveClassFallback(ClassName);
		if (!OwnerClass)
		{
			return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Class not found: %s"), *ClassName));
		}
	}
	else
	{
		// Default to Blueprint->ParentClass — the most common "self" call.
		OwnerClass = Ctx.Blueprint->ParentClass;
	}

	UFunction* Function = OwnerClass ? OwnerClass->FindFunctionByName(FName(*FunctionName)) : nullptr;
	if (!Function)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Function '%s' not found on %s"),
			*FunctionName, OwnerClass ? *OwnerClass->GetName() : TEXT("<null>")));
	}

	int32 X = 0, Y = 0;
	ReadPosition(Params, X, Y);

	UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Ctx.Graph);
	const bool bIsSelfContext = Ctx.Blueprint->GeneratedClass && Ctx.Blueprint->GeneratedClass->IsChildOf(OwnerClass);
	Node->FunctionReference.SetFromField<UFunction>(Function, bIsSelfContext);
	FinalizeNodePlacement(Ctx.Graph, Node, X, Y);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Response->SetObjectField(TEXT("node"), FBlueprintEditHelpers::NodeToEditResponse(Node));
	return Response;
}

//------------------------------------------------------------------------------
// edit.node.add_branch
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeAddBranch(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	int32 X = 0, Y = 0;
	ReadPosition(Params, X, Y);

	UK2Node_IfThenElse* Node = NewObject<UK2Node_IfThenElse>(Ctx.Graph);
	FinalizeNodePlacement(Ctx.Graph, Node, X, Y);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Response->SetObjectField(TEXT("node"), FBlueprintEditHelpers::NodeToEditResponse(Node));
	return Response;
}

//------------------------------------------------------------------------------
// edit.node.add_sequence
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeAddSequence(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	int32 X = 0, Y = 0;
	ReadPosition(Params, X, Y);

	double ThenPinCount = 2;
	Params->TryGetNumberField(TEXT("then_pin_count"), ThenPinCount);
	if (ThenPinCount < 2) { ThenPinCount = 2; }

	UK2Node_ExecutionSequence* Node = NewObject<UK2Node_ExecutionSequence>(Ctx.Graph);
	FinalizeNodePlacement(Ctx.Graph, Node, X, Y);

	// AllocateDefaultPins gave us the default two Then pins; add more to reach the count.
	for (int32 i = 2; i < (int32)ThenPinCount; ++i)
	{
		Node->AddInputPin();
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Response->SetObjectField(TEXT("node"), FBlueprintEditHelpers::NodeToEditResponse(Node));
	return Response;
}

//------------------------------------------------------------------------------
// edit.node.add_for_loop
//
// NOTE: UE4.27 doesn't have a standalone UK2Node_ForLoop; for-loops are macro
// instances referencing /Engine/EditorBlueprintResources/StandardMacros.ForLoop.
// For now this op is a stub returning an informative error — the generic
// add_node with a K2Node_MacroInstance class is the escape hatch. Full
// implementation is a good Phase E follow-up.
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeAddForLoop(const TSharedPtr<FJsonObject>& Params)
{
	return FBlueprintEditHelpers::MakeEditError(
		TEXT("add_for_loop not yet implemented — use edit.node.add_generic with K2Node_MacroInstance pointing at StandardMacros.ForLoop"));
}

//------------------------------------------------------------------------------
// edit.node.add_cast
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeAddCast(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	FString TargetClassName;
	if (!RequireString(Params, TEXT("target_class"), TargetClassName, Err)) { return Err; }

	UClass* TargetClass = ResolveClassFallback(TargetClassName);
	if (!TargetClass)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Target class not found: %s"), *TargetClassName));
	}

	int32 X = 0, Y = 0;
	ReadPosition(Params, X, Y);

	bool bPure = false;
	Params->TryGetBoolField(TEXT("pure"), bPure);

	UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(Ctx.Graph);
	Node->TargetType = TargetClass;
	Node->SetPurity(bPure);
	FinalizeNodePlacement(Ctx.Graph, Node, X, Y);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Response->SetObjectField(TEXT("node"), FBlueprintEditHelpers::NodeToEditResponse(Node));
	return Response;
}

//------------------------------------------------------------------------------
// edit.node.add_make_struct
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeAddMakeStruct(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	FString StructName;
	if (!RequireString(Params, TEXT("struct"), StructName, Err)) { return Err; }

	UScriptStruct* Struct = ResolveStructFallback(StructName);
	if (!Struct)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Struct not found: %s"), *StructName));
	}

	int32 X = 0, Y = 0;
	ReadPosition(Params, X, Y);

	UK2Node_MakeStruct* Node = NewObject<UK2Node_MakeStruct>(Ctx.Graph);
	Node->StructType = Struct;
	FinalizeNodePlacement(Ctx.Graph, Node, X, Y);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Response->SetObjectField(TEXT("node"), FBlueprintEditHelpers::NodeToEditResponse(Node));
	return Response;
}

//------------------------------------------------------------------------------
// edit.node.add_break_struct
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeAddBreakStruct(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	FString StructName;
	if (!RequireString(Params, TEXT("struct"), StructName, Err)) { return Err; }

	UScriptStruct* Struct = ResolveStructFallback(StructName);
	if (!Struct)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Struct not found: %s"), *StructName));
	}

	int32 X = 0, Y = 0;
	ReadPosition(Params, X, Y);

	UK2Node_BreakStruct* Node = NewObject<UK2Node_BreakStruct>(Ctx.Graph);
	Node->StructType = Struct;
	FinalizeNodePlacement(Ctx.Graph, Node, X, Y);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Response->SetObjectField(TEXT("node"), FBlueprintEditHelpers::NodeToEditResponse(Node));
	return Response;
}

//------------------------------------------------------------------------------
// edit.node.add_literal
//
// NOTE: UK2Node_Literal in UE4.27 is primarily used for object literal refs.
// For scalar constants (int/float/bool), agents should use edit.pin.set_default
// on an existing input pin rather than placing a dedicated literal node.
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::NodeAddLiteral(const TSharedPtr<FJsonObject>& Params)
{
	FBPGraphContext Ctx;
	TSharedPtr<FJsonObject> Err;
	if (!ResolveBPAndGraph(Params, Ctx, Err)) { return Err; }

	FString ObjectPath;
	if (!RequireString(Params, TEXT("object"), ObjectPath, Err))
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("add_literal requires an 'object' path; for scalars use edit.pin.set_default on an existing pin"));
	}

	UObject* LiteralObject = LoadObject<UObject>(nullptr, *ObjectPath);
	if (!LiteralObject)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Could not load object: %s"), *ObjectPath));
	}

	int32 X = 0, Y = 0;
	ReadPosition(Params, X, Y);

	UK2Node_Literal* Node = NewObject<UK2Node_Literal>(Ctx.Graph);
	Node->SetObjectRef(LiteralObject);
	FinalizeNodePlacement(Ctx.Graph, Node, X, Y);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(BPPath(Ctx.Blueprint));
	Response->SetObjectField(TEXT("node"), FBlueprintEditHelpers::NodeToEditResponse(Node));
	return Response;
}
