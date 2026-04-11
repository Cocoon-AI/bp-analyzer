// BlueprintEditOps_Metadata.cpp
// Phase A edit operations: lifecycle (compile/save) and blueprint-level metadata
// (reparent, interfaces, flags). These are the smallest ops in the plugin and
// serve as worked examples for the later phases.

#include "BlueprintEditOps.h"
#include "BlueprintEditHelpers.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "FileHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

//------------------------------------------------------------------------------
// Small shared param-extraction helpers (inline to keep callsites terse)
//------------------------------------------------------------------------------

namespace
{
	using FBlueprintEditHelpers::RequireString;

	// Resolve a class by path or short name. Returns nullptr on failure.
	static UClass* ResolveClass(const FString& ClassPathOrName)
	{
		if (ClassPathOrName.IsEmpty())
		{
			return nullptr;
		}

		// Full path form: /Script/Engine.Actor  or  /Game/Path/BP.BP_C
		if (ClassPathOrName.StartsWith(TEXT("/")))
		{
			if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassPathOrName))
			{
				return Loaded;
			}
			// Try appending _C for blueprint classes.
			if (!ClassPathOrName.EndsWith(TEXT("_C")))
			{
				const FString WithSuffix = ClassPathOrName + TEXT("_C");
				if (UClass* Loaded = LoadObject<UClass>(nullptr, *WithSuffix))
				{
					return Loaded;
				}
			}
			return nullptr;
		}

		// Short name: try ANY_PACKAGE, then common prefixes.
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

	// Add the current Blueprint->Status code + message to an edit response under "compile".
	static void AttachCompileStatus(const TSharedPtr<FJsonObject>& Response, UBlueprint* Blueprint)
	{
		if (!Blueprint) { return; }

		TSharedPtr<FJsonObject> CompileObj = MakeShareable(new FJsonObject);
		const TCHAR* StatusStr = TEXT("unknown");
		switch (Blueprint->Status)
		{
			case BS_Unknown:        StatusStr = TEXT("unknown"); break;
			case BS_Dirty:          StatusStr = TEXT("dirty"); break;
			case BS_Error:          StatusStr = TEXT("error"); break;
			case BS_UpToDate:       StatusStr = TEXT("up_to_date"); break;
			case BS_UpToDateWithWarnings: StatusStr = TEXT("up_to_date_with_warnings"); break;
			default: break;
		}
		CompileObj->SetStringField(TEXT("status"), StatusStr);
		Response->SetObjectField(TEXT("compile"), CompileObj);
	}
}

//------------------------------------------------------------------------------
// edit.compile
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::Compile(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint)
	{
		return FBlueprintEditHelpers::MakeEditError(LoadError);
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	AttachCompileStatus(Response, Blueprint);

	// If compile produced an error status, downgrade success to false so callers don't
	// silently ship broken blueprints.
	if (Blueprint->Status == BS_Error)
	{
		Response->SetBoolField(TEXT("success"), false);
		Response->SetStringField(TEXT("error"), TEXT("Blueprint compiled with errors; see compile.status"));
	}
	return Response;
}

//------------------------------------------------------------------------------
// edit.save
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::Save(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint)
	{
		return FBlueprintEditHelpers::MakeEditError(LoadError);
	}

	UPackage* Package = Blueprint->GetOutermost();
	if (!Package)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Blueprint has no outer package"));
	}

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);

	// bOnlyDirty=false so we save even if nothing dirtied the package through editor UI.
	// bCheckDirty=false, bPromptToSave=false — headless save.
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, /*bOnlyDirty*/false);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetBoolField(TEXT("saved"), bSaved);
	if (!bSaved)
	{
		Response->SetBoolField(TEXT("success"), false);
		Response->SetStringField(TEXT("error"), TEXT("SavePackages reported failure"));
	}
	return Response;
}

//------------------------------------------------------------------------------
// edit.save_and_compile — convenience: compile, then save
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::SaveAndCompile(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> CompileResult = Compile(Params);
	bool bCompileOk = false;
	CompileResult->TryGetBoolField(TEXT("success"), bCompileOk);
	if (!bCompileOk)
	{
		return CompileResult;
	}

	TSharedPtr<FJsonObject> SaveResult = Save(Params);
	// Merge compile info into the save result so caller gets one object with both.
	const TSharedPtr<FJsonObject>* CompileBlock = nullptr;
	if (CompileResult->TryGetObjectField(TEXT("compile"), CompileBlock) && CompileBlock)
	{
		SaveResult->SetObjectField(TEXT("compile"), *CompileBlock);
	}
	return SaveResult;
}

//------------------------------------------------------------------------------
// edit.reparent
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::Reparent(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, ParentClassName;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("parent_class"), ParentClassName, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint)
	{
		return FBlueprintEditHelpers::MakeEditError(LoadError);
	}

	UClass* NewParent = ResolveClass(ParentClassName);
	if (!NewParent)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Could not resolve parent class: %s"), *ParentClassName));
	}

	if (Blueprint->ParentClass == NewParent)
	{
		TSharedPtr<FJsonObject> NoOp = FBlueprintEditHelpers::MakeEditSuccess(Path);
		NoOp->SetBoolField(TEXT("no_op"), true);
		NoOp->SetStringField(TEXT("parent_class"), NewParent->GetPathName());
		return NoOp;
	}

	// ReparentBlueprint reassigns ParentClass and marks the blueprint structurally modified.
	// The recompile argument is handled internally; we don't force it here because the
	// agent is expected to call edit.compile explicitly.
	Blueprint->ParentClass = NewParent;
	FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("parent_class"), NewParent->GetPathName());
	return Response;
}

//------------------------------------------------------------------------------
// edit.add_interface / edit.remove_interface
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::AddInterface(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, InterfaceName;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("interface"), InterfaceName, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint)
	{
		return FBlueprintEditHelpers::MakeEditError(LoadError);
	}

	UClass* InterfaceClass = ResolveClass(InterfaceName);
	if (!InterfaceClass || !InterfaceClass->IsChildOf(UInterface::StaticClass()))
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("'%s' is not a valid interface class"), *InterfaceName));
	}

	// ImplementNewInterface takes the interface's path name as an FName.
	const FName InterfaceFName(*InterfaceClass->GetPathName());
	const bool bImplemented = FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceFName);
	if (!bImplemented)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Failed to implement interface: %s"), *InterfaceName));
	}

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("interface"), InterfaceClass->GetPathName());
	return Response;
}

TSharedPtr<FJsonObject> FBlueprintEditOps::RemoveInterface(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, InterfaceName;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("interface"), InterfaceName, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint)
	{
		return FBlueprintEditHelpers::MakeEditError(LoadError);
	}

	UClass* InterfaceClass = ResolveClass(InterfaceName);
	if (!InterfaceClass)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Could not resolve interface class: %s"), *InterfaceName));
	}

	bool bPreserveFunctions = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("preserve_functions"), bPreserveFunctions);
	}

	const FName InterfaceFName(*InterfaceClass->GetPathName());
	FBlueprintEditorUtils::RemoveInterface(Blueprint, InterfaceFName, bPreserveFunctions);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("interface"), InterfaceClass->GetPathName());
	Response->SetBoolField(TEXT("preserve_functions"), bPreserveFunctions);
	return Response;
}

//------------------------------------------------------------------------------
// edit.set_flags
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::SetFlags(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint)
	{
		return FBlueprintEditHelpers::MakeEditError(LoadError);
	}

	// All flag fields are optional — only apply the ones the caller provided.
	TArray<FString> Applied;

	bool bVal = false;
	if (Params.IsValid() && Params->TryGetBoolField(TEXT("is_abstract"), bVal))
	{
		Blueprint->bGenerateAbstractClass = bVal;
		Applied.Add(FString::Printf(TEXT("is_abstract=%s"), bVal ? TEXT("true") : TEXT("false")));
	}
	if (Params.IsValid() && Params->TryGetBoolField(TEXT("is_deprecated"), bVal))
	{
		Blueprint->bDeprecate = bVal;
		Applied.Add(FString::Printf(TEXT("is_deprecated=%s"), bVal ? TEXT("true") : TEXT("false")));
	}

	FString StrVal;
	if (Params.IsValid() && Params->TryGetStringField(TEXT("description"), StrVal))
	{
#if WITH_EDITORONLY_DATA
		Blueprint->BlueprintDescription = StrVal;
		Applied.Add(TEXT("description"));
#endif
	}
	if (Params.IsValid() && Params->TryGetStringField(TEXT("category"), StrVal))
	{
#if WITH_EDITORONLY_DATA
		Blueprint->BlueprintCategory = StrVal;
		Applied.Add(TEXT("category"));
#endif
	}

	if (Applied.Num() == 0)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("No flag fields supplied; provide at least one of: is_abstract, is_deprecated, description, category"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	TArray<TSharedPtr<FJsonValue>> AppliedArray;
	for (const FString& A : Applied)
	{
		AppliedArray.Add(MakeShareable(new FJsonValueString(A)));
	}
	Response->SetArrayField(TEXT("applied"), AppliedArray);
	return Response;
}
