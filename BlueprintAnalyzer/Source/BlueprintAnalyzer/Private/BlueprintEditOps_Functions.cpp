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
#include "K2Node_CallFunction.h"
#include "K2Node_BaseMCDelegate.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Script.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "FileHelpers.h"

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

// Walk every loaded BP under SearchPaths and retarget K2Node_CallFunction refs
// that call the removed function on the lifted BP's class. Rewrites
// FunctionReference to point at NewCppName — name-resolve then walks the parent
// chain to the new C++ UFUNCTION on the parent class. Mirrors the shape of
// LiftHelpers::RetargetExternalVarRefs; intentionally skips ReconstructNode
// (UE's compile-time reconciliation handles pin re-allocation).
struct FFunctionRetargetStats
{
	int32 NodesRetargeted = 0;
	TArray<FString> AffectedBpPaths;
};

static FFunctionRetargetStats RetargetExternalFunctionCalls(
	UBlueprint* LiftedBlueprint,
	const FName OldFuncName,
	const FName NewFuncName,
	const TArray<FString>& SearchPaths,
	TArray<UPackage*>& OutPackagesToSave)
{
	FFunctionRetargetStats Stats;
	if (!LiftedBlueprint || !LiftedBlueprint->GeneratedClass || SearchPaths.Num() == 0)
	{
		return Stats;
	}

	UClass* LiftedClass = LiftedBlueprint->GeneratedClass;
	UClass* LiftedSkelClass = LiftedBlueprint->SkeletonGeneratedClass;
	// Retarget via the BP's generated class, mirroring the variable-retarget
	// behavior. Name-resolve chain-walks to the C++ parent at compile time.

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	for (const FString& SearchPath : SearchPaths)
	{
		TArray<FAssetData> Assets;
		AR.GetAssetsByPath(FName(*SearchPath), Assets, /*bRecursive=*/true);

		for (const FAssetData& Data : Assets)
		{
			if (!Data.AssetClass.ToString().Contains(TEXT("Blueprint"))) { continue; }
			UBlueprint* BP = Cast<UBlueprint>(Data.GetAsset());
			if (!BP || !BP->GeneratedClass) { continue; }
			if (BP->GeneratedClass->HasAnyClassFlags(CLASS_NewerVersionExists)) { continue; }
			if (BP == LiftedBlueprint) { continue; }

			bool bAnyChange = false;

#if WITH_EDITORONLY_DATA
			TArray<UEdGraph*> AllGraphs;
			AllGraphs.Append(BP->FunctionGraphs);
			AllGraphs.Append(BP->UbergraphPages);
			AllGraphs.Append(BP->MacroGraphs);
			AllGraphs.Append(BP->DelegateSignatureGraphs);
			for (int32 i = 0; i < AllGraphs.Num(); ++i)
			{
				if (!AllGraphs[i]) { continue; }
				AllGraphs.Append(AllGraphs[i]->SubGraphs);
			}

			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph) { continue; }
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
					if (!CallNode) { continue; }

					const FName CurName = CallNode->FunctionReference.GetMemberName();
					if (CurName != OldFuncName) { continue; }

					// Broad match — same semantics as variable retarget.
					UClass* RefParent = CallNode->FunctionReference.GetMemberParentClass(BP->SkeletonGeneratedClass);
					const bool bIsRefToLifted =
						RefParent == nullptr ||
						RefParent == LiftedClass ||
						RefParent == LiftedSkelClass ||
						(RefParent && RefParent->ClassGeneratedBy == LiftedBlueprint);
					if (!bIsRefToLifted) { continue; }

					CallNode->FunctionReference.SetExternalMember(NewFuncName, LiftedClass);
					++Stats.NodesRetargeted;
					bAnyChange = true;
				}
			}
#endif // WITH_EDITORONLY_DATA

			if (bAnyChange)
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
				Stats.AffectedBpPaths.Add(BP->GetPathName());
				if (UPackage* Pkg = BP->GetOutermost())
				{
					OutPackagesToSave.AddUnique(Pkg);
				}
			}
		}
	}

	return Stats;
}

TSharedPtr<FJsonObject> FBlueprintEditOps::FunctionRemove(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Name;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("name"), Name, Err)) { return Err; }

	// Optional: retarget external K2Node_CallFunction refs from (BPClass, Name)
	// to (BPClass, retarget_to) after the remove. Use when a BP function has
	// been lifted to a C++ UFUNCTION with a different name (e.g. "Play SFX"
	// display-name lifted to C++ PlaySFX) — without retargeting, external
	// callers orphan with "Could not find a function named 'Play SFX'".
	FString RetargetTo;
	Params->TryGetStringField(TEXT("retarget_external_to"), RetargetTo);

	bool bNoScanExternal = false;
	Params->TryGetBoolField(TEXT("no_scan_external"), bNoScanExternal);

	TArray<FString> ExternalSearchPaths;
	const TArray<TSharedPtr<FJsonValue>>* ScopeArray = nullptr;
	if (Params->TryGetArrayField(TEXT("scope"), ScopeArray) && ScopeArray)
	{
		for (const TSharedPtr<FJsonValue>& V : *ScopeArray)
		{
			ExternalSearchPaths.Add(V->AsString());
		}
	}
	if (ExternalSearchPaths.Num() == 0)
	{
		ExternalSearchPaths.Add(TEXT("/Game/"));
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	UEdGraph* Graph = FindFunctionGraph(Blueprint, FName(*Name));
	if (!Graph)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Function '%s' not found"), *Name));
	}

	FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Recompile);

	int32 ExternalNodesRetargeted = 0;
	TArray<TSharedPtr<FJsonValue>> AffectedBpsJson;
	TArray<FString> SaveFailures;
	if (!RetargetTo.IsEmpty() && !bNoScanExternal)
	{
		TArray<UPackage*> PackagesToSave;
		FFunctionRetargetStats Stats = RetargetExternalFunctionCalls(
			Blueprint, FName(*Name), FName(*RetargetTo), ExternalSearchPaths, PackagesToSave);
		ExternalNodesRetargeted = Stats.NodesRetargeted;
		for (const FString& BpPath : Stats.AffectedBpPaths)
		{
			AffectedBpsJson.Add(MakeShareable(new FJsonValueString(BpPath)));
		}
		if (PackagesToSave.Num() > 0)
		{
			const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, /*bOnlyDirty=*/true);
			if (!bSaved)
			{
				SaveFailures.Add(TEXT("One or more retargeted external BP packages failed to save"));
			}
		}
	}

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("name"), Name);
	if (!RetargetTo.IsEmpty())
	{
		Response->SetStringField(TEXT("retarget_external_to"), RetargetTo);
		Response->SetBoolField(TEXT("scanned_external"), !bNoScanExternal);
		Response->SetNumberField(TEXT("external_nodes_retargeted"), ExternalNodesRetargeted);
		Response->SetNumberField(TEXT("external_bps_affected"), AffectedBpsJson.Num());
		Response->SetArrayField(TEXT("external_bps"), AffectedBpsJson);
	}
	if (SaveFailures.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const FString& W : SaveFailures)
		{
			Warnings.Add(MakeShareable(new FJsonValueString(W)));
		}
		Response->SetArrayField(TEXT("warnings"), Warnings);
	}
	return Response;
}

//------------------------------------------------------------------------------
// edit.external.rewrite_call
//
// Cross-BP K2Node_CallFunction rewrite, non-destructive. Walks --scope for
// nodes with FunctionReference == (OldClass, OldName) and rewrites them to
// (NewClass, NewName). Same retarget shape as `function remove
// --retarget-external-to`, but without removing or otherwise touching the
// source function — purely an external-callsite update.
//
// Use cases: a C++ UFUNCTION is being renamed (LoadInventory → FetchInventory)
// or moved to a different class (USDPlayFabClientSubsystem::Get →
// USDClientSubsystem::Get), and the BP-side K2Node_CallFunction sites need to
// follow.
//------------------------------------------------------------------------------

namespace ExternalRewriteHelpers
{
	// Mirrors FBlueprintEditHelpers::FindClassByName from BlueprintEditHelpers.cpp,
	// but lives in this file's anonymous-equivalent namespace to avoid a unity-
	// build static-fn collision (the helper there is also `static`).
	static UClass* FindClassByName_Ext(const FString& Name)
	{
		if (Name.IsEmpty()) { return nullptr; }
		if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *Name)) { return Found; }
		if (UClass* Loaded = LoadObject<UClass>(nullptr, *Name)) { return Loaded; }
		for (const TCHAR* Prefix : { TEXT("U"), TEXT("A") })
		{
			if (!Name.StartsWith(Prefix))
			{
				const FString Prefixed = FString(Prefix) + Name;
				if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *Prefixed)) { return Found; }
			}
		}
		return nullptr;
	}
}

TSharedPtr<FJsonObject> FBlueprintEditOps::ExternalRewriteCall(const TSharedPtr<FJsonObject>& Params)
{
	FString OldClassName, OldFuncName, NewFuncName;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("old_class"), OldClassName, Err)) { return Err; }
	if (!RequireString(Params, TEXT("old_name"), OldFuncName, Err)) { return Err; }
	if (!RequireString(Params, TEXT("new_name"), NewFuncName, Err)) { return Err; }

	// new_class is optional; defaults to old_class for the pure-rename case.
	FString NewClassName;
	Params->TryGetStringField(TEXT("new_class"), NewClassName);
	if (NewClassName.IsEmpty()) { NewClassName = OldClassName; }

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	bool bNoScanExternal = false;
	Params->TryGetBoolField(TEXT("no_scan_external"), bNoScanExternal);

	TArray<FString> SearchPaths;
	const TArray<TSharedPtr<FJsonValue>>* ScopeArray = nullptr;
	if (Params->TryGetArrayField(TEXT("scope"), ScopeArray) && ScopeArray)
	{
		for (const TSharedPtr<FJsonValue>& V : *ScopeArray)
		{
			SearchPaths.Add(V->AsString());
		}
	}
	if (SearchPaths.Num() == 0)
	{
		SearchPaths.Add(TEXT("/Game/"));
	}

	// Resolve classes. Both must exist before we touch anything; safer to fail
	// fast than partial-rewrite some BPs and then discover the new class
	// doesn't exist.
	UClass* OldClass = ExternalRewriteHelpers::FindClassByName_Ext(OldClassName);
	if (!OldClass)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(
			TEXT("Could not resolve old_class '%s' (try the full name like 'USDPlayFabClientSubsystem')"), *OldClassName));
	}
	UClass* NewClass = ExternalRewriteHelpers::FindClassByName_Ext(NewClassName);
	if (!NewClass)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(
			TEXT("Could not resolve new_class '%s'"), *NewClassName));
	}

	const FName OldFName(*OldFuncName);
	const FName NewFName(*NewFuncName);

	int32 NodesRetargeted = 0;
	TArray<FString> AffectedBpPaths;
	TArray<UPackage*> PackagesToSave;
	TArray<FString> SaveFailures;

	if (!bNoScanExternal)
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();

		for (const FString& SearchPath : SearchPaths)
		{
			TArray<FAssetData> Assets;
			AR.GetAssetsByPath(FName(*SearchPath), Assets, /*bRecursive=*/true);

			for (const FAssetData& Data : Assets)
			{
				if (!Data.AssetClass.ToString().Contains(TEXT("Blueprint"))) { continue; }
				UBlueprint* BP = Cast<UBlueprint>(Data.GetAsset());
				if (!BP || !BP->GeneratedClass) { continue; }
				if (BP->GeneratedClass->HasAnyClassFlags(CLASS_NewerVersionExists)) { continue; }

				bool bAnyChange = false;

#if WITH_EDITORONLY_DATA
				TArray<UEdGraph*> AllGraphs;
				AllGraphs.Append(BP->FunctionGraphs);
				AllGraphs.Append(BP->UbergraphPages);
				AllGraphs.Append(BP->MacroGraphs);
				AllGraphs.Append(BP->DelegateSignatureGraphs);
				for (int32 i = 0; i < AllGraphs.Num(); ++i)
				{
					if (!AllGraphs[i]) { continue; }
					AllGraphs.Append(AllGraphs[i]->SubGraphs);
				}

				for (UEdGraph* Graph : AllGraphs)
				{
					if (!Graph) { continue; }
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
						if (!CallNode) { continue; }

						const FName CurName = CallNode->FunctionReference.GetMemberName();
						if (CurName != OldFName) { continue; }

						UClass* RefParent = CallNode->FunctionReference.GetMemberParentClass(BP->SkeletonGeneratedClass);
						// Match against OldClass and its skeleton/REINST variants —
						// the same broad match used for the lift path. A stricter
						// match (RefParent == OldClass) misses hot-reload / SKEL_
						// proxies, which are the same logical class.
						const bool bIsRefToOld =
							RefParent == OldClass ||
							(OldClass->ClassGeneratedBy && RefParent && RefParent->ClassGeneratedBy == OldClass->ClassGeneratedBy) ||
							(RefParent && RefParent->GetName().Contains(OldClass->GetName()));
						if (!bIsRefToOld) { continue; }

						if (!bDryRun)
						{
							CallNode->FunctionReference.SetExternalMember(NewFName, NewClass);
						}
						++NodesRetargeted;
						bAnyChange = true;
					}
				}
#endif // WITH_EDITORONLY_DATA

				if (bAnyChange && !bDryRun)
				{
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
					AffectedBpPaths.Add(BP->GetPathName());
					if (UPackage* Pkg = BP->GetOutermost())
					{
						PackagesToSave.AddUnique(Pkg);
					}
				}
				else if (bAnyChange && bDryRun)
				{
					AffectedBpPaths.Add(BP->GetPathName());
				}
			}
		}

		if (!bDryRun && PackagesToSave.Num() > 0)
		{
			const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, /*bOnlyDirty=*/true);
			if (!bSaved)
			{
				SaveFailures.Add(TEXT("One or more rewritten BP packages failed to save"));
			}
		}
	}

	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
	Response->SetBoolField(TEXT("success"), true);
	Response->SetBoolField(TEXT("dry_run"), bDryRun);
	Response->SetStringField(TEXT("old_class"), OldClassName);
	Response->SetStringField(TEXT("old_name"), OldFuncName);
	Response->SetStringField(TEXT("new_class"), NewClassName);
	Response->SetStringField(TEXT("new_name"), NewFuncName);
	Response->SetBoolField(TEXT("scanned_external"), !bNoScanExternal);
	Response->SetNumberField(TEXT("external_nodes_retargeted"), NodesRetargeted);
	Response->SetNumberField(TEXT("external_bps_affected"), AffectedBpPaths.Num());

	TArray<TSharedPtr<FJsonValue>> BpsJson;
	for (const FString& P : AffectedBpPaths)
	{
		BpsJson.Add(MakeShareable(new FJsonValueString(P)));
	}
	Response->SetArrayField(TEXT("external_bps"), BpsJson);

	if (SaveFailures.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const FString& W : SaveFailures)
		{
			Warnings.Add(MakeShareable(new FJsonValueString(W)));
		}
		Response->SetArrayField(TEXT("warnings"), Warnings);
	}
	return Response;
}

//------------------------------------------------------------------------------
// edit.external.rewrite_delegate
//
// Cross-BP K2Node_BaseMCDelegate rewrite (Add/Remove/Clear/Call/Assign of a
// BlueprintAssignable multicast delegate). Same shape as rewrite_call but
// targets DelegateReference rather than FunctionReference. Use when a C++
// multicast delegate UPROPERTY is being renamed and BP K2Node refs need to
// follow without removing the source.
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::ExternalRewriteDelegate(const TSharedPtr<FJsonObject>& Params)
{
	FString OldClassName, OldDelegateName, NewDelegateName;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("old_class"), OldClassName, Err)) { return Err; }
	if (!RequireString(Params, TEXT("old_name"), OldDelegateName, Err)) { return Err; }
	if (!RequireString(Params, TEXT("new_name"), NewDelegateName, Err)) { return Err; }

	FString NewClassName;
	Params->TryGetStringField(TEXT("new_class"), NewClassName);
	if (NewClassName.IsEmpty()) { NewClassName = OldClassName; }

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	bool bNoScanExternal = false;
	Params->TryGetBoolField(TEXT("no_scan_external"), bNoScanExternal);

	TArray<FString> SearchPaths;
	const TArray<TSharedPtr<FJsonValue>>* ScopeArray = nullptr;
	if (Params->TryGetArrayField(TEXT("scope"), ScopeArray) && ScopeArray)
	{
		for (const TSharedPtr<FJsonValue>& V : *ScopeArray)
		{
			SearchPaths.Add(V->AsString());
		}
	}
	if (SearchPaths.Num() == 0)
	{
		SearchPaths.Add(TEXT("/Game/"));
	}

	UClass* OldClass = ExternalRewriteHelpers::FindClassByName_Ext(OldClassName);
	if (!OldClass)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(
			TEXT("Could not resolve old_class '%s'"), *OldClassName));
	}
	UClass* NewClass = ExternalRewriteHelpers::FindClassByName_Ext(NewClassName);
	if (!NewClass)
	{
		return FBlueprintEditHelpers::MakeEditError(FString::Printf(
			TEXT("Could not resolve new_class '%s'"), *NewClassName));
	}

	const FName OldFName(*OldDelegateName);
	const FName NewFName(*NewDelegateName);

	int32 NodesRetargeted = 0;
	TArray<FString> AffectedBpPaths;
	TArray<UPackage*> PackagesToSave;
	TArray<FString> SaveFailures;

	if (!bNoScanExternal)
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();

		for (const FString& SearchPath : SearchPaths)
		{
			TArray<FAssetData> Assets;
			AR.GetAssetsByPath(FName(*SearchPath), Assets, /*bRecursive=*/true);

			for (const FAssetData& Data : Assets)
			{
				if (!Data.AssetClass.ToString().Contains(TEXT("Blueprint"))) { continue; }
				UBlueprint* BP = Cast<UBlueprint>(Data.GetAsset());
				if (!BP || !BP->GeneratedClass) { continue; }
				if (BP->GeneratedClass->HasAnyClassFlags(CLASS_NewerVersionExists)) { continue; }

				bool bAnyChange = false;

#if WITH_EDITORONLY_DATA
				TArray<UEdGraph*> AllGraphs;
				AllGraphs.Append(BP->FunctionGraphs);
				AllGraphs.Append(BP->UbergraphPages);
				AllGraphs.Append(BP->MacroGraphs);
				AllGraphs.Append(BP->DelegateSignatureGraphs);
				for (int32 i = 0; i < AllGraphs.Num(); ++i)
				{
					if (!AllGraphs[i]) { continue; }
					AllGraphs.Append(AllGraphs[i]->SubGraphs);
				}

				for (UEdGraph* Graph : AllGraphs)
				{
					if (!Graph) { continue; }
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						// UK2Node_BaseMCDelegate is the common base for
						// AddDelegate / RemoveDelegate / ClearDelegate /
						// CallDelegate / AssignDelegate (and CreateDelegate
						// derives from a different base, not this one).
						UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(Node);
						if (!DelegateNode) { continue; }

						const FName CurName = DelegateNode->DelegateReference.GetMemberName();
						if (CurName != OldFName) { continue; }

						UClass* RefParent = DelegateNode->DelegateReference.GetMemberParentClass(BP->SkeletonGeneratedClass);
						const bool bIsRefToOld =
							RefParent == OldClass ||
							(OldClass->ClassGeneratedBy && RefParent && RefParent->ClassGeneratedBy == OldClass->ClassGeneratedBy) ||
							(RefParent && RefParent->GetName().Contains(OldClass->GetName()));
						if (!bIsRefToOld) { continue; }

						if (!bDryRun)
						{
							DelegateNode->DelegateReference.SetExternalMember(NewFName, NewClass);
						}
						++NodesRetargeted;
						bAnyChange = true;
					}
				}
#endif // WITH_EDITORONLY_DATA

				if (bAnyChange && !bDryRun)
				{
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
					AffectedBpPaths.Add(BP->GetPathName());
					if (UPackage* Pkg = BP->GetOutermost())
					{
						PackagesToSave.AddUnique(Pkg);
					}
				}
				else if (bAnyChange && bDryRun)
				{
					AffectedBpPaths.Add(BP->GetPathName());
				}
			}
		}

		if (!bDryRun && PackagesToSave.Num() > 0)
		{
			const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, /*bOnlyDirty=*/true);
			if (!bSaved)
			{
				SaveFailures.Add(TEXT("One or more rewritten BP packages failed to save"));
			}
		}
	}

	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
	Response->SetBoolField(TEXT("success"), true);
	Response->SetBoolField(TEXT("dry_run"), bDryRun);
	Response->SetStringField(TEXT("old_class"), OldClassName);
	Response->SetStringField(TEXT("old_name"), OldDelegateName);
	Response->SetStringField(TEXT("new_class"), NewClassName);
	Response->SetStringField(TEXT("new_name"), NewDelegateName);
	Response->SetBoolField(TEXT("scanned_external"), !bNoScanExternal);
	Response->SetNumberField(TEXT("external_nodes_retargeted"), NodesRetargeted);
	Response->SetNumberField(TEXT("external_bps_affected"), AffectedBpPaths.Num());

	TArray<TSharedPtr<FJsonValue>> BpsJson;
	for (const FString& P : AffectedBpPaths)
	{
		BpsJson.Add(MakeShareable(new FJsonValueString(P)));
	}
	Response->SetArrayField(TEXT("external_bps"), BpsJson);

	if (SaveFailures.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const FString& W : SaveFailures)
		{
			Warnings.Add(MakeShareable(new FJsonValueString(W)));
		}
		Response->SetArrayField(TEXT("warnings"), Warnings);
	}
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
