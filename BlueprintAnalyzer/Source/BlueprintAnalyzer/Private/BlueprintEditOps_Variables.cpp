// BlueprintEditOps_Variables.cpp
// Phase B edit operations: blueprint member variables (CRUD + flags + metadata)
// and CDO property get/set for parent-class properties.

#include "BlueprintEditOps.h"
#include "BlueprintEditHelpers.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/UserDefinedStruct.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"
#include "UObject/TextProperty.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

//------------------------------------------------------------------------------
// Local helpers
//------------------------------------------------------------------------------

namespace
{
	using FBlueprintEditHelpers::RequireString;

	// Returns true when the pin type's sub-object reference is broken (deleted struct/class/enum).
	// Named with EditOps_ prefix to avoid unity-build collisions with the reader's copy.
	static bool EditOps_IsPinTypeBroken(const FEdGraphPinType& PinType)
	{
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

	// Convert FEdGraphPinType to a human-readable string (mirrors the reader's PinTypeToString).
	static FString EditOps_PinTypeToString(const FEdGraphPinType& PinType)
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
		if (PinType.ContainerType == EPinContainerType::Array)      { Result = TEXT("TArray<") + Result + TEXT(">"); }
		else if (PinType.ContainerType == EPinContainerType::Set)   { Result = TEXT("TSet<") + Result + TEXT(">"); }
		else if (PinType.ContainerType == EPinContainerType::Map)   { Result = TEXT("TMap<") + Result + TEXT(">"); }
		if (PinType.bIsReference) { Result += TEXT("&"); }
		if (PinType.bIsConst)     { Result = TEXT("const ") + Result; }
		return Result;
	}

	// Find a variable description by name. Returns -1 if not found.
	static int32 FindVariableIndex(UBlueprint* Blueprint, const FName& VarName)
	{
		for (int32 i = 0; i < Blueprint->NewVariables.Num(); ++i)
		{
			if (Blueprint->NewVariables[i].VarName == VarName)
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

	// Translate a replication condition string to the enum. Returns true on recognized name.
	static bool ParseReplicationCondition(const FString& Name, ELifetimeCondition& OutCondition)
	{
		static const TMap<FString, ELifetimeCondition> Map = {
			{ TEXT("None"),                  COND_None },
			{ TEXT("InitialOnly"),           COND_InitialOnly },
			{ TEXT("OwnerOnly"),             COND_OwnerOnly },
			{ TEXT("SkipOwner"),             COND_SkipOwner },
			{ TEXT("SimulatedOnly"),         COND_SimulatedOnly },
			{ TEXT("AutonomousOnly"),        COND_AutonomousOnly },
			{ TEXT("SimulatedOrPhysics"),    COND_SimulatedOrPhysics },
			{ TEXT("InitialOrOwner"),        COND_InitialOrOwner },
			{ TEXT("Custom"),                COND_Custom },
			{ TEXT("ReplayOrOwner"),         COND_ReplayOrOwner },
			{ TEXT("ReplayOnly"),            COND_ReplayOnly },
			{ TEXT("SimulatedOnlyNoReplay"), COND_SimulatedOnlyNoReplay },
			{ TEXT("SimulatedOrPhysicsNoReplay"), COND_SimulatedOrPhysicsNoReplay },
			{ TEXT("SkipReplay"),            COND_SkipReplay },
			{ TEXT("Never"),                 COND_Never },
		};
		if (const ELifetimeCondition* Found = Map.Find(Name))
		{
			OutCondition = *Found;
			return true;
		}
		return false;
	}
}

//------------------------------------------------------------------------------
// edit.variable.list
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::VariableList(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }

	bool bIncludeBroken = false;
	Params->TryGetBoolField(TEXT("include_broken"), bIncludeBroken);

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	TArray<TSharedPtr<FJsonValue>> VarArray;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		const bool bBroken = EditOps_IsPinTypeBroken(Var.VarType);
		if (bBroken && !bIncludeBroken) { continue; }

		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
		Obj->SetStringField(TEXT("name"), Var.VarName.ToString());
		Obj->SetStringField(TEXT("type"), EditOps_PinTypeToString(Var.VarType));
		Obj->SetStringField(TEXT("category"), Var.Category.ToString());
		Obj->SetStringField(TEXT("default_value"), Var.DefaultValue);
		Obj->SetBoolField(TEXT("is_public"), (Var.PropertyFlags & CPF_BlueprintVisible) != 0);
		Obj->SetBoolField(TEXT("is_replicated"), (Var.PropertyFlags & CPF_Net) != 0);
		if (bBroken)
		{
			Obj->SetBoolField(TEXT("is_type_broken"), true);
			Obj->SetStringField(TEXT("pin_category"), Var.VarType.PinCategory.ToString());
		}
		VarArray.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetArrayField(TEXT("variables"), VarArray);
	Response->SetNumberField(TEXT("count"), VarArray.Num());
	return Response;
}

//------------------------------------------------------------------------------
// edit.variable.add
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::VariableAdd(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name, TypeStr;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }
	if (!RequireString(Params, TEXT("type"), TypeStr, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint)
	{
		return FBlueprintEditHelpers::MakeEditError(LoadError);
	}

	const FName VarName(*Name);
	if (FindVariableIndex(Blueprint, VarName) != INDEX_NONE)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Variable '%s' already exists"), *Name));
	}

	FEdGraphPinType PinType;
	FString ParseError;
	if (!FBlueprintEditHelpers::ParsePinType(TypeStr, PinType, ParseError))
	{
		return FBlueprintEditHelpers::MakeEditError(ParseError);
	}

	const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType);
	if (!bAdded)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("AddMemberVariable failed for '%s'"), *Name));
	}

	// Apply optional post-creation fields.
	const int32 NewIdx = FindVariableIndex(Blueprint, VarName);
	if (NewIdx == INDEX_NONE)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Internal: variable added but not found in NewVariables"));
	}
	FBPVariableDescription& Desc = Blueprint->NewVariables[NewIdx];

	FString StrVal;
	if (Params->TryGetStringField(TEXT("default_value"), StrVal))
	{
		Desc.DefaultValue = StrVal;
	}
	if (Params->TryGetStringField(TEXT("category"), StrVal) && !StrVal.IsEmpty())
	{
		FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VarName, nullptr, FText::FromString(StrVal));
	}
	if (Params->TryGetStringField(TEXT("tooltip"), StrVal))
	{
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, TEXT("tooltip"), StrVal);
	}

	bool bBoolVal = false;
	if (Params->TryGetBoolField(TEXT("is_public"), bBoolVal))
	{
		if (bBoolVal) { Desc.PropertyFlags |= CPF_BlueprintVisible; Desc.PropertyFlags |= CPF_Edit; }
		else          { Desc.PropertyFlags &= ~CPF_Edit; }
	}
	if (Params->TryGetBoolField(TEXT("is_readonly"), bBoolVal))
	{
		if (bBoolVal) { Desc.PropertyFlags |= CPF_BlueprintReadOnly; }
		else          { Desc.PropertyFlags &= ~CPF_BlueprintReadOnly; }
	}
	if (Params->TryGetBoolField(TEXT("is_replicated"), bBoolVal))
	{
		if (bBoolVal) { Desc.PropertyFlags |= CPF_Net; }
		else          { Desc.PropertyFlags &= ~CPF_Net; }
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	Response->SetStringField(TEXT("type"), TypeStr);
	return Response;
}

//------------------------------------------------------------------------------
// edit.variable.remove
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::VariableRemove(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }

	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	const FName VarName(*Name);
	const int32 Idx = FindVariableIndex(Blueprint, VarName);

	if (Idx != INDEX_NONE)
	{
		if (bForce)
		{
			// Direct array removal — bypasses FBlueprintEditorUtils which may choke
			// on variables whose type object has been deleted.
			Blueprint->NewVariables.RemoveAt(Idx);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		else
		{
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);
		}
	}
	else
	{
		return FBlueprintEditHelpers::MakeEditError(
			FString::Printf(TEXT("Variable '%s' not found (try --force for phantom variables)"), *Name));
	}

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	if (bForce) { Response->SetBoolField(TEXT("forced"), true); }
	return Response;
}

//------------------------------------------------------------------------------
// edit.variable.rename
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::VariableRename(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, OldName, NewName;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("old_name"), OldName, Err)) { return Err; }
	if (!RequireString(Params, TEXT("new_name"), NewName, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	const FName OldVarName(*OldName);
	const FName NewVarName(*NewName);
	if (FindVariableIndex(Blueprint, OldVarName) == INDEX_NONE)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Variable '%s' not found"), *OldName));
	}
	if (FindVariableIndex(Blueprint, NewVarName) != INDEX_NONE)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Variable '%s' already exists"), *NewName));
	}

	FBlueprintEditorUtils::RenameMemberVariable(Blueprint, OldVarName, NewVarName);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("old_name"), OldName);
	Response->SetStringField(TEXT("new_name"), NewName);
	return Response;
}

//------------------------------------------------------------------------------
// edit.variable.set_type
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::VariableSetType(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name, TypeStr;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }
	if (!RequireString(Params, TEXT("type"), TypeStr, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	const FName VarName(*Name);
	if (FindVariableIndex(Blueprint, VarName) == INDEX_NONE)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Variable '%s' not found"), *Name));
	}

	FEdGraphPinType NewType;
	FString ParseError;
	if (!FBlueprintEditHelpers::ParsePinType(TypeStr, NewType, ParseError))
	{
		return FBlueprintEditHelpers::MakeEditError(ParseError);
	}

	FBlueprintEditorUtils::ChangeMemberVariableType(Blueprint, VarName, NewType);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	Response->SetStringField(TEXT("type"), TypeStr);
	return Response;
}

//------------------------------------------------------------------------------
// edit.variable.set_default
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::VariableSetDefault(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name, NewValue;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }
	if (!Params->TryGetStringField(TEXT("default_value"), NewValue))
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Missing required param: default_value"));
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	const FName VarName(*Name);
	const int32 Idx = FindVariableIndex(Blueprint, VarName);
	if (Idx == INDEX_NONE)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Variable '%s' not found"), *Name));
	}

	Blueprint->NewVariables[Idx].DefaultValue = NewValue;
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	Response->SetStringField(TEXT("default_value"), NewValue);
	return Response;
}

//------------------------------------------------------------------------------
// edit.variable.set_flags
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::VariableSetFlags(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	const FName VarName(*Name);
	const int32 Idx = FindVariableIndex(Blueprint, VarName);
	if (Idx == INDEX_NONE)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Variable '%s' not found"), *Name));
	}

	FBPVariableDescription& Desc = Blueprint->NewVariables[Idx];
	TArray<FString> Applied;

	bool bBoolVal = false;
	if (Params->TryGetBoolField(TEXT("is_public"), bBoolVal))
	{
		if (bBoolVal) { Desc.PropertyFlags |= CPF_BlueprintVisible; Desc.PropertyFlags |= CPF_Edit; }
		else          { Desc.PropertyFlags &= ~CPF_BlueprintVisible; Desc.PropertyFlags &= ~CPF_Edit; }
		Applied.Add(TEXT("is_public"));
	}
	if (Params->TryGetBoolField(TEXT("is_readonly"), bBoolVal))
	{
		if (bBoolVal) { Desc.PropertyFlags |= CPF_BlueprintReadOnly; }
		else          { Desc.PropertyFlags &= ~CPF_BlueprintReadOnly; }
		Applied.Add(TEXT("is_readonly"));
	}
	if (Params->TryGetBoolField(TEXT("is_replicated"), bBoolVal))
	{
		if (bBoolVal) { Desc.PropertyFlags |= CPF_Net; }
		else          { Desc.PropertyFlags &= ~CPF_Net; }
		Applied.Add(TEXT("is_replicated"));
	}

	FString CondStr;
	if (Params->TryGetStringField(TEXT("replication_condition"), CondStr))
	{
		ELifetimeCondition Cond = COND_None;
		if (!ParseReplicationCondition(CondStr, Cond))
		{
			return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Unknown replication_condition: %s"), *CondStr));
		}
		Desc.ReplicationCondition = Cond;
		Applied.Add(TEXT("replication_condition"));
	}

	if (Applied.Num() == 0)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("No flag fields supplied"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	TArray<TSharedPtr<FJsonValue>> AppliedArr;
	for (const FString& A : Applied) { AppliedArr.Add(MakeShareable(new FJsonValueString(A))); }
	Response->SetArrayField(TEXT("applied"), AppliedArr);
	return Response;
}

//------------------------------------------------------------------------------
// edit.variable.set_metadata
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::VariableSetMetadata(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name, Key, Value;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }
	if (!RequireString(Params, TEXT("key"), Key, Err)) { return Err; }
	// Value may be empty (clearing metadata), so use TryGet without emptiness check.
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Missing required param: value"));
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	const FName VarName(*Name);
	if (FindVariableIndex(Blueprint, VarName) == INDEX_NONE)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Variable '%s' not found"), *Name));
	}

	FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FName(*Key), Value);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	Response->SetStringField(TEXT("key"), Key);
	Response->SetStringField(TEXT("value"), Value);
	return Response;
}

//------------------------------------------------------------------------------
// edit.cdo.set_property
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::CdoSetProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, PropertyName, Value;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("property_name"), PropertyName, Err)) { return Err; }
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Missing required param: value"));
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	if (!Blueprint->GeneratedClass)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Blueprint has no GeneratedClass; compile it first"));
	}

	UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
	if (!CDO)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Could not obtain CDO"));
	}

	FProperty* Prop = CDO->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Property not found on CDO: %s"), *PropertyName));
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
	// UE4.27: FProperty::ImportText(Buffer, Data, PortFlags, OwnerObject, ErrorText)
	const TCHAR* ImportResult = Prop->ImportText(*Value, ValuePtr, PPF_None, CDO);
	if (ImportResult == nullptr)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("ImportText failed for value: %s"), *Value));
	}

	// Ensure the blueprint is re-marked so the next compile propagates the CDO change.
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("property_name"), PropertyName);
	Response->SetStringField(TEXT("value"), Value);
	return Response;
}

//------------------------------------------------------------------------------
// edit.cdo.get_property
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::CdoGetProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, PropertyName;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("property_name"), PropertyName, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	if (!Blueprint->GeneratedClass)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Blueprint has no GeneratedClass; compile it first"));
	}

	UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
	if (!CDO)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Could not obtain CDO"));
	}

	FProperty* Prop = CDO->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Property not found on CDO: %s"), *PropertyName));
	}

	FString Exported;
	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
	// UE4.27: FProperty::ExportTextItem(ValueStr, PropertyValue, DefaultValue, Parent, PortFlags)
	Prop->ExportTextItem(Exported, ValuePtr, /*DefaultValue*/nullptr, CDO, PPF_None);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("property_name"), PropertyName);
	Response->SetStringField(TEXT("value"), Exported);
	Response->SetStringField(TEXT("property_type"), Prop->GetCPPType());
	return Response;
}

//------------------------------------------------------------------------------
// edit.purge_phantom
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::PurgePhantom(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, PropName;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("property"), PropName, Err)) { return Err; }

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	const FName PropFName(*PropName);
	TArray<TSharedPtr<FJsonValue>> Actions;
	bool bNeedsRecompile = false;

	// 1. Check NewVariables for the name.
	{
		const int32 Idx = FindVariableIndex(Blueprint, PropFName);
		if (Idx != INDEX_NONE)
		{
			TSharedPtr<FJsonObject> Act = MakeShareable(new FJsonObject);
			Act->SetStringField(TEXT("location"), TEXT("NewVariables"));
			Act->SetNumberField(TEXT("index"), Idx);
			Act->SetStringField(TEXT("type"), EditOps_PinTypeToString(Blueprint->NewVariables[Idx].VarType));
			Act->SetBoolField(TEXT("is_type_broken"), EditOps_IsPinTypeBroken(Blueprint->NewVariables[Idx].VarType));
			if (!bDryRun)
			{
				Blueprint->NewVariables.RemoveAt(Idx);
				bNeedsRecompile = true;
				Act->SetBoolField(TEXT("removed"), true);
			}
			Actions.Add(MakeShareable(new FJsonValueObject(Act)));
		}
	}

	// 2. Check GeneratedClass for the stale FProperty.
	if (Blueprint->GeneratedClass)
	{
		FProperty* Prop = Blueprint->GeneratedClass->FindPropertyByName(PropFName);
		if (Prop)
		{
			TSharedPtr<FJsonObject> Act = MakeShareable(new FJsonObject);
			Act->SetStringField(TEXT("location"), TEXT("GeneratedClass"));
			Act->SetStringField(TEXT("property_type"), Prop->GetCPPType());
			Act->SetStringField(TEXT("note"), TEXT("Stale compiled property — will be removed by full recompile"));
			Actions.Add(MakeShareable(new FJsonValueObject(Act)));
			bNeedsRecompile = true;
		}
	}

	// 4. Force full recompile if anything was found and not dry-run.
	if (bNeedsRecompile && !bDryRun)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("property"), PropName);
	Response->SetArrayField(TEXT("actions"), Actions);
	Response->SetNumberField(TEXT("locations_found"), Actions.Num());
	if (bDryRun) { Response->SetBoolField(TEXT("dry_run"), true); }
	if (bNeedsRecompile && !bDryRun) { Response->SetBoolField(TEXT("recompiled"), true); }

	if (Actions.Num() == 0)
	{
		Response->SetStringField(TEXT("note"), TEXT("Property not found in NewVariables, LocalVariables, or GeneratedClass"));
	}

	return Response;
}
