// BlueprintEditOps_Components.cpp
// Phase C edit operations for Simple Construction Script (SCS) components:
// add, remove, reparent, and set-property on component templates.

#include "BlueprintEditOps.h"
#include "BlueprintEditHelpers.h"

#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/SceneComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	using FBlueprintEditHelpers::RequireString;

	static UClass* ResolveComponentClass(const FString& ClassPathOrName)
	{
		if (ClassPathOrName.IsEmpty()) { return nullptr; }
		if (ClassPathOrName.StartsWith(TEXT("/")))
		{
			return LoadObject<UClass>(nullptr, *ClassPathOrName);
		}
		if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *ClassPathOrName))
		{
			return Found;
		}
		for (const TCHAR* Prefix : { TEXT("U"), TEXT("A") })
		{
			if (!ClassPathOrName.StartsWith(Prefix))
			{
				const FString Prefixed = FString(Prefix) + ClassPathOrName;
				if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *Prefixed))
				{
					return Found;
				}
			}
		}
		return nullptr;
	}

	// Detach a node from its parent (if any) within the SCS.
	static void DetachFromParent(USimpleConstructionScript* SCS, USCS_Node* Node)
	{
		if (!SCS || !Node) { return; }
		// If it's a root node, remove from roots.
		if (SCS->GetRootNodes().Contains(Node))
		{
			SCS->RemoveNode(Node);
			return;
		}
		// Otherwise find the parent node and remove it as a child.
		for (USCS_Node* Candidate : SCS->GetAllNodes())
		{
			if (Candidate && Candidate->GetChildNodes().Contains(Node))
			{
				Candidate->RemoveChildNode(Node);
				return;
			}
		}
	}
}

//------------------------------------------------------------------------------
// edit.component.add
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::ComponentAdd(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name, ClassName;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }
	if (!RequireString(Params, TEXT("class"), ClassName, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	if (!Blueprint->SimpleConstructionScript)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Blueprint has no SimpleConstructionScript (not an Actor Blueprint?)"));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;

	if (SCS->FindSCSNode(FName(*Name)) != nullptr)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Component '%s' already exists"), *Name));
	}

	UClass* ComponentClass = ResolveComponentClass(ClassName);
	if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("'%s' is not a valid UActorComponent class"), *ClassName));
	}

	USCS_Node* NewNode = SCS->CreateNode(ComponentClass, FName(*Name));
	if (!NewNode)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("SCS->CreateNode returned null"));
	}

	FString ParentName;
	if (Params->TryGetStringField(TEXT("parent"), ParentName) && !ParentName.IsEmpty())
	{
		USCS_Node* ParentNode = SCS->FindSCSNode(FName(*ParentName));
		if (!ParentNode)
		{
			return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Parent component '%s' not found"), *ParentName));
		}
		ParentNode->AddChildNode(NewNode);
	}
	else
	{
		SCS->AddNode(NewNode);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	Response->SetStringField(TEXT("class"), ComponentClass->GetPathName());
	if (!ParentName.IsEmpty()) { Response->SetStringField(TEXT("parent"), ParentName); }
	return Response;
}

//------------------------------------------------------------------------------
// edit.component.remove
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::ComponentRemove(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	if (!Blueprint->SimpleConstructionScript)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	USCS_Node* Node = SCS->FindSCSNode(FName(*Name));
	if (!Node)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Component '%s' not found"), *Name));
	}

	bool bPromoteChildren = true;
	Params->TryGetBoolField(TEXT("promote_children"), bPromoteChildren);

	if (bPromoteChildren)
	{
		SCS->RemoveNodeAndPromoteChildren(Node);
	}
	else
	{
		SCS->RemoveNode(Node);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	Response->SetBoolField(TEXT("promote_children"), bPromoteChildren);
	return Response;
}

//------------------------------------------------------------------------------
// edit.component.reparent
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::ComponentReparent(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name, NewParentName;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }
	if (!RequireString(Params, TEXT("new_parent"), NewParentName, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	if (!Blueprint->SimpleConstructionScript)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	USCS_Node* Node = SCS->FindSCSNode(FName(*Name));
	if (!Node)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Component '%s' not found"), *Name));
	}
	USCS_Node* NewParent = SCS->FindSCSNode(FName(*NewParentName));
	if (!NewParent)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Parent component '%s' not found"), *NewParentName));
	}
	if (Node == NewParent)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Component cannot be parented to itself"));
	}

	DetachFromParent(SCS, Node);
	NewParent->AddChildNode(Node);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	Response->SetStringField(TEXT("new_parent"), NewParentName);
	return Response;
}

//------------------------------------------------------------------------------
// edit.component.set_property
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::ComponentSetProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Component, PropertyName, Value;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("component"), Component, Err)) { return Err; }
	if (!RequireString(Params, TEXT("property"), PropertyName, Err)) { return Err; }
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Missing required param: value"));
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	if (!Blueprint->SimpleConstructionScript)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	USCS_Node* Node = Blueprint->SimpleConstructionScript->FindSCSNode(FName(*Component));
	if (!Node || !Node->ComponentTemplate)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Component '%s' not found or has no template"), *Component));
	}

	UObject* Template = Node->ComponentTemplate;
	FProperty* Prop = Template->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *Template->GetClass()->GetName()));
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Template);
	// UE4.27: FProperty::ImportText(Buffer, Data, PortFlags, OwnerObject, ErrorText)
	const TCHAR* ImportResult = Prop->ImportText(*Value, ValuePtr, PPF_None, Template);
	if (ImportResult == nullptr)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("ImportText failed for value: %s"), *Value));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("component"), Component);
	Response->SetStringField(TEXT("property"), PropertyName);
	Response->SetStringField(TEXT("value"), Value);
	return Response;
}
