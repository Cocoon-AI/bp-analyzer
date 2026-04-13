// BlueprintEditOps_Functions.cpp
// Phase C edit operations for functions and events (ubergraph custom events and
// inherited event implementations).

#include "BlueprintEditOps.h"
#include "BlueprintEditHelpers.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Script.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

//------------------------------------------------------------------------------
// Local helpers
//------------------------------------------------------------------------------

namespace
{
	using FBlueprintEditHelpers::RequireString;

	// Find a function graph by name. Returns nullptr if not found.
	static UEdGraph* FindFunctionGraph(UBlueprint* Blueprint, const FName& FunctionName)
	{
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == FunctionName)
			{
				return Graph;
			}
		}
		return nullptr;
	}

	// Locate the function entry node in a function graph. Returns nullptr if not found.
	static UK2Node_FunctionEntry* FindFunctionEntry(UEdGraph* Graph)
	{
		if (!Graph) { return nullptr; }
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				return Entry;
			}
		}
		return nullptr;
	}

	// Locate the function result node in a function graph. Returns nullptr if none exists.
	// Functions without outputs may not have a result node yet.
	static UK2Node_FunctionResult* FindFunctionResult(UEdGraph* Graph)
	{
		if (!Graph) { return nullptr; }
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
			{
				return Result;
			}
		}
		return nullptr;
	}

	// Walk UserDefinedPins directly. UE4.27's UK2Node_EditablePinBase declares
	// UserDefinedPinExists and RemoveUserDefinedPinByName but doesn't export them
	// from the BlueprintGraph module, so we can't call them from here. The
	// UserDefinedPins array itself is public UPROPERTY storage, and
	// RemoveUserDefinedPin(TSharedPtr) *is* BLUEPRINTGRAPH_API exported.
	static TSharedPtr<FUserPinInfo> FindUserDefinedPin(UK2Node_EditablePinBase* Node, const FName& PinName)
	{
		if (!Node) { return nullptr; }
		for (const TSharedPtr<FUserPinInfo>& Info : Node->UserDefinedPins)
		{
			if (Info.IsValid() && Info->PinName == PinName)
			{
				return Info;
			}
		}
		return nullptr;
	}
}

//------------------------------------------------------------------------------
// edit.function.add
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::FunctionAdd(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	const FName FuncName(*Name);
	if (FindFunctionGraph(Blueprint, FuncName) != nullptr)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Function '%s' already exists"), *Name));
	}

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, FuncName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!NewGraph)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("CreateNewGraph failed"));
	}

	// bIsUserCreated=true, signature=nullptr (empty user function).
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, /*bIsUserCreated*/true, nullptr);

	bool bIsPure = false;
	bool bIsConst = false;
	Params->TryGetBoolField(TEXT("is_pure"), bIsPure);
	Params->TryGetBoolField(TEXT("is_const"), bIsConst);

	if (bIsPure || bIsConst)
	{
		if (UK2Node_FunctionEntry* Entry = FindFunctionEntry(NewGraph))
		{
			if (bIsPure)  { Entry->AddExtraFlags(FUNC_BlueprintPure); }
			if (bIsConst) { Entry->AddExtraFlags(FUNC_Const); }
		}
	}

	FString Category;
	if (Params->TryGetStringField(TEXT("category"), Category) && !Category.IsEmpty())
	{
		if (UK2Node_FunctionEntry* Entry = FindFunctionEntry(NewGraph))
		{
			Entry->MetaData.Category = FText::FromString(Category);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	Response->SetBoolField(TEXT("is_pure"), bIsPure);
	Response->SetBoolField(TEXT("is_const"), bIsConst);
	return Response;
}

//------------------------------------------------------------------------------
// edit.function.remove
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::FunctionRemove(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	UEdGraph* Graph = FindFunctionGraph(Blueprint, FName(*Name));
	if (!Graph)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Function '%s' not found"), *Name));
	}

	FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Recompile);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	return Response;
}

//------------------------------------------------------------------------------
// edit.function.rename
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::FunctionRename(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, OldName, NewName;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("old_name"), OldName, Err)) { return Err; }
	if (!RequireString(Params, TEXT("new_name"), NewName, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	UEdGraph* Graph = FindFunctionGraph(Blueprint, FName(*OldName));
	if (!Graph)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Function '%s' not found"), *OldName));
	}
	if (FindFunctionGraph(Blueprint, FName(*NewName)) != nullptr)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Function '%s' already exists"), *NewName));
	}

	FBlueprintEditorUtils::RenameGraph(Graph, NewName);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("old_name"), OldName);
	Response->SetStringField(TEXT("new_name"), NewName);
	return Response;
}

//------------------------------------------------------------------------------
// edit.function.add_param / remove_param
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::FunctionAddParam(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Function, Name, TypeStr, Direction;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("function"), Function, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }
	if (!RequireString(Params, TEXT("type"), TypeStr, Err)) { return Err; }
	if (!RequireString(Params, TEXT("direction"), Direction, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	UEdGraph* Graph = FindFunctionGraph(Blueprint, FName(*Function));
	if (!Graph)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Function '%s' not found"), *Function));
	}

	FEdGraphPinType PinType;
	FString ParseError;
	if (!FBlueprintEditHelpers::ParsePinType(TypeStr, PinType, ParseError))
	{
		return FBlueprintEditHelpers::MakeEditError(ParseError);
	}

	const bool bInput = Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase);
	const bool bOutput = Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase);
	if (!bInput && !bOutput)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("direction must be 'input' or 'output'"));
	}

	if (bInput)
	{
		UK2Node_FunctionEntry* Entry = FindFunctionEntry(Graph);
		if (!Entry)
		{
			return FBlueprintEditHelpers::MakeEditError(TEXT("Function entry node not found"));
		}
		UEdGraphPin* NewPin = Entry->CreateUserDefinedPin(FName(*Name), PinType, EGPD_Output);
		if (!NewPin)
		{
			return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("CreateUserDefinedPin failed for input '%s'"), *Name));
		}
	}
	else
	{
		// For outputs we may need to create a result node if none exists. AddFunctionGraph
		// does not create one by default — it's added lazily when the first output is set.
		UK2Node_FunctionResult* Result = FindFunctionResult(Graph);
		if (!Result)
		{
			// Use the schema's helper to create the result node automatically via the
			// function entry's auto-generated return path. Simplest path: spawn directly.
			Result = NewObject<UK2Node_FunctionResult>(Graph);
			Result->CreateNewGuid();
			Result->PostPlacedNewNode();
			Result->SetFlags(RF_Transactional);
			Result->AllocateDefaultPins();
			Result->NodePosX = 400;
			Result->NodePosY = 0;
			Graph->AddNode(Result, /*bFromUI*/false, /*bSelectNewNode*/false);
		}
		UEdGraphPin* NewPin = Result->CreateUserDefinedPin(FName(*Name), PinType, EGPD_Input);
		if (!NewPin)
		{
			return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("CreateUserDefinedPin failed for output '%s'"), *Name));
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("function"), Function);
	Response->SetStringField(TEXT("name"), Name);
	Response->SetStringField(TEXT("type"), TypeStr);
	Response->SetStringField(TEXT("direction"), bInput ? TEXT("input") : TEXT("output"));
	return Response;
}

TSharedPtr<FJsonObject> FBlueprintEditOps::FunctionRemoveParam(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Function, Name;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("function"), Function, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	UEdGraph* Graph = FindFunctionGraph(Blueprint, FName(*Function));
	if (!Graph)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Function '%s' not found"), *Function));
	}

	const FName PinFName(*Name);
	bool bRemoved = false;
	if (UK2Node_FunctionEntry* Entry = FindFunctionEntry(Graph))
	{
		if (TSharedPtr<FUserPinInfo> Info = FindUserDefinedPin(Entry, PinFName))
		{
			Entry->RemoveUserDefinedPin(Info);
			bRemoved = true;
		}
	}
	if (!bRemoved)
	{
		if (UK2Node_FunctionResult* Result = FindFunctionResult(Graph))
		{
			if (TSharedPtr<FUserPinInfo> Info = FindUserDefinedPin(Result, PinFName))
			{
				Result->RemoveUserDefinedPin(Info);
				bRemoved = true;
			}
		}
	}
	if (!bRemoved)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Param '%s' not found on function '%s'"), *Name, *Function));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("function"), Function);
	Response->SetStringField(TEXT("name"), Name);
	return Response;
}

//------------------------------------------------------------------------------
// edit.function.set_flags
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::FunctionSetFlags(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Function;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("function"), Function, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	UEdGraph* Graph = FindFunctionGraph(Blueprint, FName(*Function));
	if (!Graph)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Function '%s' not found"), *Function));
	}

	UK2Node_FunctionEntry* Entry = FindFunctionEntry(Graph);
	if (!Entry)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Function entry node not found"));
	}

	TArray<FString> Applied;
	bool bVal = false;
	if (Params->TryGetBoolField(TEXT("is_pure"), bVal))
	{
		if (bVal)
		{
			Entry->AddExtraFlags(FUNC_BlueprintPure);
		}
		else
		{
			Entry->ClearExtraFlags(FUNC_BlueprintPure);
		}
		Applied.Add(TEXT("is_pure"));
	}
	if (Params->TryGetBoolField(TEXT("is_const"), bVal))
	{
		if (bVal)
		{
			Entry->AddExtraFlags(FUNC_Const);
		}
		else
		{
			Entry->ClearExtraFlags(FUNC_Const);
		}
		Applied.Add(TEXT("is_const"));
	}

	if (Applied.Num() == 0)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("No flag fields supplied"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("function"), Function);
	TArray<TSharedPtr<FJsonValue>> AppliedArr;
	for (const FString& A : Applied) { AppliedArr.Add(MakeShareable(new FJsonValueString(A))); }
	Response->SetArrayField(TEXT("applied"), AppliedArr);
	return Response;
}

//------------------------------------------------------------------------------
// edit.function.override
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::FunctionOverride(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Function;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("function"), Function, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	if (!Blueprint->ParentClass)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Blueprint has no parent class"));
	}

	UFunction* ParentFunc = Blueprint->ParentClass->FindFunctionByName(FName(*Function));
	if (!ParentFunc)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Parent class has no function '%s'"), *Function));
	}

	if (FindFunctionGraph(Blueprint, FName(*Function)) != nullptr)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Function '%s' already overridden"), *Function));
	}

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, FName(*Function), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!NewGraph)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("CreateNewGraph failed"));
	}

	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, NewGraph, /*bIsUserCreated*/false, ParentFunc);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("function"), Function);
	return Response;
}

//------------------------------------------------------------------------------
// edit.event.add_custom
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::EventAddCustom(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	if (Blueprint->UbergraphPages.Num() == 0)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Blueprint has no event graph (UbergraphPages is empty)"));
	}
	UEdGraph* Graph = Blueprint->UbergraphPages[0];

	UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(Graph);
	EventNode->CustomFunctionName = FName(*Name);
	EventNode->CreateNewGuid();
	EventNode->PostPlacedNewNode();
	EventNode->SetFlags(RF_Transactional);
	EventNode->AllocateDefaultPins();

	int32 PosX = 0, PosY = 0;
	double NumVal;
	if (Params->TryGetNumberField(TEXT("x"), NumVal)) { PosX = (int32)NumVal; }
	if (Params->TryGetNumberField(TEXT("y"), NumVal)) { PosY = (int32)NumVal; }
	EventNode->NodePosX = PosX;
	EventNode->NodePosY = PosY;

	Graph->AddNode(EventNode, /*bFromUI*/false, /*bSelectNewNode*/false);

	// Add any custom parameters: params is an array of {name, type}.
	const TArray<TSharedPtr<FJsonValue>>* ParamArr = nullptr;
	if (Params->TryGetArrayField(TEXT("params"), ParamArr) && ParamArr)
	{
		for (const TSharedPtr<FJsonValue>& Val : *ParamArr)
		{
			const TSharedPtr<FJsonObject>* ParamObj = nullptr;
			if (!Val->TryGetObject(ParamObj) || !ParamObj) { continue; }

			FString PName, PType;
			if (!(*ParamObj)->TryGetStringField(TEXT("name"), PName) || PName.IsEmpty()) { continue; }
			if (!(*ParamObj)->TryGetStringField(TEXT("type"), PType) || PType.IsEmpty()) { continue; }

			FEdGraphPinType PinType;
			FString ParseErr;
			if (!FBlueprintEditHelpers::ParsePinType(PType, PinType, ParseErr))
			{
				return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Param '%s': %s"), *PName, *ParseErr));
			}
			EventNode->CreateUserDefinedPin(FName(*PName), PinType, EGPD_Output);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	Response->SetStringField(TEXT("node_guid"), EventNode->NodeGuid.ToString());
	return Response;
}

//------------------------------------------------------------------------------
// edit.event.remove
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::EventRemove(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	UEdGraphNode* Found = nullptr;
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph) { continue; }
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Node))
			{
				if (Custom->CustomFunctionName == FName(*Name))
				{
					Found = Custom;
					break;
				}
			}
			else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				if (EventNode->EventReference.GetMemberName() == FName(*Name))
				{
					Found = EventNode;
					break;
				}
			}
		}
		if (Found) { break; }
	}
	if (!Found)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Event '%s' not found"), *Name));
	}

	FBlueprintEditorUtils::RemoveNode(Blueprint, Found, /*bDontRecompile*/true);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	return Response;
}

//------------------------------------------------------------------------------
// edit.event.implement
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::EventImplement(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, EventName;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("event"), EventName, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	if (!Blueprint->ParentClass)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Blueprint has no parent class"));
	}

	UFunction* ParentFunc = Blueprint->ParentClass->FindFunctionByName(FName(*EventName));
	if (!ParentFunc)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Parent class has no event '%s'"), *EventName));
	}
	if (!ParentFunc->HasAnyFunctionFlags(FUNC_BlueprintEvent))
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("'%s' is not a BlueprintEvent"), *EventName));
	}

	if (Blueprint->UbergraphPages.Num() == 0)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Blueprint has no event graph"));
	}
	UEdGraph* Graph = Blueprint->UbergraphPages[0];

	// Check for pre-existing implementation.
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			if (EventNode->EventReference.GetMemberName() == FName(*EventName))
			{
				return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Event '%s' already implemented"), *EventName));
			}
		}
	}

	UK2Node_Event* NewEventNode = NewObject<UK2Node_Event>(Graph);
	NewEventNode->EventReference.SetExternalMember(FName(*EventName), Blueprint->ParentClass);
	NewEventNode->bOverrideFunction = true;
	NewEventNode->CreateNewGuid();
	NewEventNode->PostPlacedNewNode();
	NewEventNode->SetFlags(RF_Transactional);
	NewEventNode->AllocateDefaultPins();
	Graph->AddNode(NewEventNode, /*bFromUI*/false, /*bSelectNewNode*/false);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("event"), EventName);
	Response->SetStringField(TEXT("node_guid"), NewEventNode->NodeGuid.ToString());
	return Response;
}

//------------------------------------------------------------------------------
// edit.dispatcher.remove
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::DispatcherRemove(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name;
	TSharedPtr<FJsonObject> Err;
	if (!FBlueprintEditHelpers::RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!FBlueprintEditHelpers::RequireString(Params, TEXT("name"), Name, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	// Find the delegate signature graph by name.
	UEdGraph* FoundGraph = nullptr;
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (Graph && Graph->GetName() == Name)
		{
			FoundGraph = Graph;
			break;
		}
	}

	if (!FoundGraph)
	{
		return FBlueprintEditHelpers::MakeEditError(
			FString::Printf(TEXT("Event dispatcher '%s' not found in DelegateSignatureGraphs"), *Name));
	}

	// Remove the delegate graph. This also removes the corresponding multicast
	// delegate variable from NewVariables.
	FBlueprintEditorUtils::RemoveGraph(Blueprint, FoundGraph);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	return Response;
}
