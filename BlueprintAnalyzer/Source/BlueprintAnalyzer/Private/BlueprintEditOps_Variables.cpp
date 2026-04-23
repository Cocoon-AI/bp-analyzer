// BlueprintEditOps_Variables.cpp
// Phase B edit operations: blueprint member variables (CRUD + flags + metadata)
// and CDO property get/set for parent-class properties.

#include "BlueprintEditOps.h"
#include "BlueprintEditHelpers.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/UserDefinedStruct.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_Variable.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"
#include "UObject/TextProperty.h"
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
// edit.variable.unshadow
//
// Recovery from the "_0 shadow" trap: when a BP author adds a C++ UPROPERTY to
// the parent class while the BP still has a member variable of the same name,
// UE renames the BP var to `<X>_0` *and* retargets every K2Node_VariableGet/Set
// that referenced it to the new `_0` name. That rewrite is persisted into the
// .uasset — only recovery used to be `p4 revert` + redo.
//
// This op walks the BP, detects every NewVariable named `<X>_0` whose base name
// `<X>` exists on the parent class (as a UPROPERTY or multicast delegate), and:
//   (a) retargets every K2Node_Variable* node referencing `<X>_0` to reference `<X>`
//   (b) retargets every K2Node_BaseMCDelegate* similarly
//   (c) removes the `<X>_0` BP variables
//   (d) marks the BP structurally modified (caller compiles separately)
//
// Idempotent: running on an already-clean BP produces zero actions and success.
//------------------------------------------------------------------------------

namespace UnshadowHelpers
{
	// Returns true if ShadowName is "<Base>_0" for some non-empty Base.
	// Strict "_0" suffix for v1 — UE's shadow rename starts at _0 and only
	// climbs to _1, _2 when prior suffixes already exist, which is vanishingly
	// rare in the lift workflow this op targets.
	static bool TrySplitShadowName(const FName& ShadowName, FName& OutBase)
	{
		const FString S = ShadowName.ToString();
		static const TCHAR* Suffix = TEXT("_0");
		if (!S.EndsWith(Suffix)) { return false; }
		const FString Base = S.LeftChop(FString(Suffix).Len());
		if (Base.IsEmpty()) { return false; }
		OutBase = FName(*Base);
		return true;
	}

	// Enumerate every UPROPERTY + multicast delegate on the parent chain into a
	// set of FNames. Used to decide whether a `<X>_0` shadow has a real `<X>`
	// counterpart on the parent (only those qualify for retargeting).
	static void CollectParentSymbolNames(UClass* ParentClass, TSet<FName>& OutNames)
	{
		for (UClass* Cls = ParentClass; Cls; Cls = Cls->GetSuperClass())
		{
			if (Cls == UObject::StaticClass()) { break; }
			for (TFieldIterator<FProperty> It(Cls, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				OutNames.Add(It->GetFName());
			}
		}
	}
}

TSharedPtr<FJsonObject> FBlueprintEditOps::VariableUnshadow(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	UClass* ParentClass = Blueprint->ParentClass;
	if (!ParentClass)
	{
		return FBlueprintEditHelpers::MakeEditError(
			TEXT("Blueprint has no parent class to unshadow against"));
	}

	// Build <parent symbol> set so we only treat `<X>_0` as a shadow when `<X>`
	// actually exists on the parent — prevents blindly renaming arbitrary `_0`
	// variables into other BPs' logic.
	TSet<FName> ParentSymbols;
	UnshadowHelpers::CollectParentSymbolNames(ParentClass, ParentSymbols);

	// shadow name -> base name. TMap-of-FName is fine here; count is small.
	TMap<FName, FName> ShadowMap;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		FName Base;
		if (UnshadowHelpers::TrySplitShadowName(Var.VarName, Base) && ParentSymbols.Contains(Base))
		{
			ShadowMap.Add(Var.VarName, Base);
		}
	}

	TArray<TSharedPtr<FJsonValue>> DetectedJson;
	for (const TPair<FName, FName>& Pair : ShadowMap)
	{
		TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
		Item->SetStringField(TEXT("shadow_name"), Pair.Key.ToString());
		Item->SetStringField(TEXT("parent_name"), Pair.Value.ToString());
		DetectedJson.Add(MakeShareable(new FJsonValueObject(Item)));
	}

	// Count intended retargets even on dry-run so the caller knows the blast radius.
	int32 NodesRetargeted = 0;

#if WITH_EDITORONLY_DATA
	TArray<UEdGraph*> AllGraphs;
	AllGraphs.Append(Blueprint->FunctionGraphs);
	AllGraphs.Append(Blueprint->UbergraphPages);
	AllGraphs.Append(Blueprint->MacroGraphs);
	AllGraphs.Append(Blueprint->DelegateSignatureGraphs);

	// Recurse into SubGraphs: collapsed-to-function nodes and nested composites
	// hold their body graphs in UEdGraph::SubGraphs, not at the BP top-level.
	// Previously these were missed — a gamedev report on 2026-04-21 saw 3
	// K2Node_VariableGet nodes skipped inside function-graph subgraphs.
	for (int32 i = 0; i < AllGraphs.Num(); ++i)
	{
		if (!AllGraphs[i]) { continue; }
		AllGraphs.Append(AllGraphs[i]->SubGraphs);
	}

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			// Variable get/set retargeting
			if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
			{
				const FName CurName = VarNode->VariableReference.GetMemberName();
				if (const FName* NewName = ShadowMap.Find(CurName))
				{
					if (!bDryRun)
					{
						// Self-scope — the target property lives on the BP's parent
						// chain, and self-scope refs resolve through the generated
						// class. Matches how inherited UPROPERTY refs normally look.
						VarNode->VariableReference.SetSelfMember(*NewName);
						VarNode->ReconstructNode();
					}
					++NodesRetargeted;
				}
				continue;
			}

			// Delegate bind/call retargeting (Add/Remove/Clear/Call/Assign — all derive
			// from UK2Node_BaseMCDelegate and expose DelegateReference)
			if (UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(Node))
			{
				const FName CurName = DelegateNode->DelegateReference.GetMemberName();
				if (const FName* NewName = ShadowMap.Find(CurName))
				{
					if (!bDryRun)
					{
						DelegateNode->DelegateReference.SetSelfMember(*NewName);
						DelegateNode->ReconstructNode();
					}
					++NodesRetargeted;
				}
				continue;
			}
		}
	}
#endif // WITH_EDITORONLY_DATA

	int32 VarsRemoved = 0;
	if (!bDryRun && ShadowMap.Num() > 0)
	{
		// Remove shadow vars by iterating backwards to avoid index invalidation.
		for (int32 i = Blueprint->NewVariables.Num() - 1; i >= 0; --i)
		{
			if (ShadowMap.Contains(Blueprint->NewVariables[i].VarName))
			{
				Blueprint->NewVariables.RemoveAt(i);
				++VarsRemoved;
			}
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetBoolField(TEXT("dry_run"), bDryRun);
	Response->SetArrayField(TEXT("shadows_detected"), DetectedJson);
	Response->SetNumberField(TEXT("nodes_retargeted"), NodesRetargeted);
	Response->SetNumberField(TEXT("vars_removed"), VarsRemoved);
	return Response;
}

//------------------------------------------------------------------------------
// edit.variable.lift
//
// Atomic multi-var lift: for each requested var, rename to a C++-identifier-
// friendly form (strip spaces, keep existing casing) and remove. Single RPC,
// single structural modification pass, caller compiles separately. Exists so
// the stage-1 BP→C++ var-lift can't partially apply if the middle step errors.
//
// --dry-run returns the plan without mutating anything.
// Returns per-var final name, plus any requested names not found in the BP.
//------------------------------------------------------------------------------

namespace LiftHelpers
{
	// Strip spaces + tabs and upper-case the first char of each whitespace-
	// separated segment. Keeps existing casing of every non-first char so
	// "XP Level Threshold" → "XPLevelThreshold" and "currentXP" → "CurrentXP".
	static FString ToCppIdentifier(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		bool bAtSegmentStart = true;
		for (TCHAR C : In)
		{
			if (FChar::IsWhitespace(C))
			{
				bAtSegmentStart = true;
				continue;
			}
			if (bAtSegmentStart)
			{
				Out.AppendChar(FChar::ToUpper(C));
				bAtSegmentStart = false;
			}
			else
			{
				Out.AppendChar(C);
			}
		}
		return Out;
	}

	// Walk every loaded BP under SearchPaths and retarget K2Node_VariableGet/Set
	// that reference the lifted BP's (OldName, *Class) via MemberReference.
	// External callers were bound to the PRE-rename name; without this, their
	// pins orphan silently once the lifted BP loses the old name.
	//
	// Rewrites the member name to NewName (keeping the BP class as the parent,
	// so FMemberReference resolution walks to the C++ parent chain where the
	// new UPROPERTY now lives). Marks affected BPs structurally modified and
	// collects their packages for batch save.
	struct FExternalRetargetStats
	{
		int32 NodesRetargeted = 0;
		int32 NodesRefreshed = 0;
		TArray<FString> AffectedBpPaths;
	};

	// Run edit.node.refresh_variables equivalent on a BP: ReconstructNode every
	// K2Node_Variable / CallFunction / BaseMCDelegate. Duplicates the logic
	// from FBlueprintEditOps::NodeRefreshVariables instead of cross-calling
	// because we're inside the lift walk and already hold the loaded BP ptr.
	static void RefreshVarsLikeNodesOnBp(UBlueprint* BP, int32& OutCount)
	{
		if (!BP) { return; }
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
			TArray<UEdGraphNode*> NodesCopy = Graph->Nodes;
			for (UEdGraphNode* Node : NodesCopy)
			{
				if (!Node) { continue; }
				if (Cast<UK2Node_Variable>(Node) ||
				    Cast<UK2Node_CallFunction>(Node) ||
				    Cast<UK2Node_BaseMCDelegate>(Node))
				{
					Node->ReconstructNode();
					++OutCount;
				}
			}
		}
#endif // WITH_EDITORONLY_DATA
	}

	// VarsToProcess: OldName -> FinalName. Populated with every lifted var
	// (renamed OR same-name removed). Non-renamed removes still need the
	// pin-link-preservation pass because UE's own re-resolve at external-BP
	// compile time narrows the K2Node_Variable Target pin from the lifted BP
	// class to its C++ parent and drops the upcast-compatible link — same root
	// cause as the rename case, just deferred to the next external compile.
	static FExternalRetargetStats RetargetExternalVarRefs(
		UBlueprint* LiftedBlueprint,
		const TMap<FName, FName>& VarsToProcess,
		const TMap<FName, FName>& RenameMap,
		const TArray<FString>& SearchPaths,
		bool bRefreshAfterRetarget,
		TArray<UPackage*>& OutPackagesToSave)
	{
		FExternalRetargetStats Stats;
		if (VarsToProcess.Num() == 0 || SearchPaths.Num() == 0 || !LiftedBlueprint || !LiftedBlueprint->GeneratedClass)
		{
			return Stats;
		}

		UClass* LiftedClass = LiftedBlueprint->GeneratedClass;
		UClass* LiftedSkelClass = LiftedBlueprint->SkeletonGeneratedClass;
		// Keep MemberParentClass = LiftedClass (BP's own generated class). The
		// gamedev v4 canary showed that pointing MemberParentClass at the C++
		// parent didn't actually fix the Target-pin-loss issue (the real unlock
		// is post-lift `edit node refresh-variables` — see --refresh-external-
		// after-lift) AND broke the external_bps_affected count reporting. So
		// we're back to the v3 behavior: retarget via the child class, let
		// FMemberReference chain-walk to the parent at compile time.

		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();

		for (const FString& SearchPath : SearchPaths)
		{
			TArray<FAssetData> Assets;
			AR.GetAssetsByPath(FName(*SearchPath), Assets, /*bRecursive=*/true);

			for (const FAssetData& Data : Assets)
			{
				if (!Data.AssetClass.ToString().Contains(TEXT("Blueprint")))
				{
					continue;
				}

				UBlueprint* BP = Cast<UBlueprint>(Data.GetAsset());
				if (!BP || !BP->GeneratedClass) { continue; }
				if (BP->GeneratedClass->HasAnyClassFlags(CLASS_NewerVersionExists)) { continue; }
				if (BP == LiftedBlueprint) { continue; } // lifted BP's internal retarget handled by RenameMemberVariable

				bool bAnyChange = false;

#if WITH_EDITORONLY_DATA
				TArray<UEdGraph*> AllGraphs;
				AllGraphs.Append(BP->FunctionGraphs);
				AllGraphs.Append(BP->UbergraphPages);
				AllGraphs.Append(BP->MacroGraphs);
				AllGraphs.Append(BP->DelegateSignatureGraphs);

				// Recurse into SubGraphs so collapsed-to-function nodes + nested
				// composites don't hide K2Node_Variable refs from the walk.
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
						// --- K2Node_Variable retarget ---
						// Just update the FMemberReference and mark the BP modified.
						// Previously tried ReconstructNode + manual pin-link restore
						// via schema.TryCreateConnection — didn't survive UE's own
						// compile-time ReconstructAllNodes on the external BP, which
						// runs its own pin migration and can fight our in-memory
						// reconstruction. Trust UE's compile-time flow: FMemberRef
						// is now correct, UE's name-match carryover handles the rest
						// when the external BP is next compiled. If the upcast link
						// still drops, it's an engine-level pin-migration limit
						// (K2Node_Variable doesn't preserve child→parent object pin
						// links when the resolved property owner moves cross-class).
						if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
						{
							const FName CurName = VarNode->VariableReference.GetMemberName();
							const FName* FinalName = VarsToProcess.Find(CurName);
							if (!FinalName) { continue; }

							// Broad ref-to-lifted match: accepts the lifted BP's
							// generated class, its skeleton class, any class whose
							// ClassGeneratedBy is the lifted BP (covers REINST_ /
							// hot-reload artifacts), and a null RefParent fallback
							// where the stored ref couldn't be resolved (trust the
							// name match to filter false positives, since lifted
							// names are user-scoped).
							UClass* RefParent = VarNode->VariableReference.GetMemberParentClass(BP->SkeletonGeneratedClass);
							const bool bIsRefToLifted =
								RefParent == nullptr ||
								RefParent == LiftedClass ||
								RefParent == LiftedSkelClass ||
								(RefParent && RefParent->ClassGeneratedBy == LiftedBlueprint);
							if (!bIsRefToLifted) { continue; }

							// Retarget via the BP's own generated class (not the C++
							// parent — v4 experiment showed that broke counting
							// without fixing the Target-pin issue, which is
							// actually unlocked by post-lift refresh-variables).
							VarNode->VariableReference.SetExternalMember(*FinalName, LiftedClass);

							++Stats.NodesRetargeted;
							bAnyChange = true;
							continue;
						}

						// --- K2Node_ConstructObjectFromClass (CreateWidget, SpawnActor, ...) ---
						// Auto-generate input pins from the target class's
						// ExposeOnSpawn UPROPERTYs. When we rename a var (e.g.
						// "Is Locked" -> "IsLocked"), the node's old pin name
						// doesn't match any UPROPERTY anymore, so ReconstructNode
						// drops it — and UE's name-match carryover (which is
						// what should preserve serialized DefaultValue on the
						// renamed pin) apparently doesn't fire reliably for
						// this node class. Gamedev's v5 canary surfaced "Is
						// Locked=true" values getting reset to false (UE default)
						// on external CreateWidget callers.
						//
						// Fix: snapshot DefaultValue/DefaultObject/DefaultTextValue/
						// AutogeneratedDefaultValue/LinkedTo BEFORE the rename +
						// reconstruct, then re-apply to the reconstructed pin by
						// new name. Handles both scalar defaults and link
						// preservation in one pass.
						if (UK2Node_ConstructObjectFromClass* SpawnNode = Cast<UK2Node_ConstructObjectFromClass>(Node))
						{
							UEdGraphPin* ClassPin = SpawnNode->GetClassPin();
							if (!ClassPin) { continue; }
							UClass* SpawnClass = Cast<UClass>(ClassPin->DefaultObject);
							if (!SpawnClass) { continue; }

							// Match spawn-target against the lifted class chain. Any
							// subclass of the lifted BP that inherits the renamed
							// UPROPERTYs still exposes them via the spawn pin set.
							const bool bSpawnsLifted =
								SpawnClass == LiftedClass ||
								SpawnClass == LiftedSkelClass ||
								SpawnClass->IsChildOf(LiftedClass) ||
								(LiftedSkelClass && SpawnClass->IsChildOf(LiftedSkelClass));
							if (!bSpawnsLifted) { continue; }

							struct FPinValueSnapshot
							{
								FName NewName;
								EEdGraphPinDirection Direction;
								FString DefaultValue;
								TWeakObjectPtr<UObject> DefaultObject;
								FText DefaultTextValue;
								FString AutogeneratedDefaultValue;
								TArray<UEdGraphPin*> LinkedTo;
							};
							TArray<FPinValueSnapshot> Snapshots;

							for (UEdGraphPin* Pin : SpawnNode->Pins)
							{
								if (!Pin) { continue; }
								const FName* NewName = RenameMap.Find(Pin->PinName);
								if (!NewName) { continue; }

								FPinValueSnapshot Snap;
								Snap.NewName = *NewName;
								Snap.Direction = Pin->Direction;
								Snap.DefaultValue = Pin->DefaultValue;
								Snap.DefaultObject = Pin->DefaultObject;
								Snap.DefaultTextValue = Pin->DefaultTextValue;
								Snap.AutogeneratedDefaultValue = Pin->AutogeneratedDefaultValue;
								Snap.LinkedTo = Pin->LinkedTo;
								Snapshots.Add(MoveTemp(Snap));

								Pin->PinName = *NewName;
								++Stats.NodesRetargeted;
							}
							if (Snapshots.Num() > 0)
							{
								SpawnNode->ReconstructNode();

								// Re-apply serialized pin state to the new pins
								// by name. If the new pin isn't there (UPROPERTY
								// didn't exist on the parent after all), the
								// snapshot is silently dropped — same effective
								// state as before the fix.
								for (const FPinValueSnapshot& Snap : Snapshots)
								{
									UEdGraphPin* NewPin = nullptr;
									for (UEdGraphPin* P : SpawnNode->Pins)
									{
										if (P && P->PinName == Snap.NewName && P->Direction == Snap.Direction)
										{
											NewPin = P;
											break;
										}
									}
									if (!NewPin) { continue; }
									NewPin->DefaultValue = Snap.DefaultValue;
									NewPin->DefaultObject = Snap.DefaultObject.Get();
									NewPin->DefaultTextValue = Snap.DefaultTextValue;
									NewPin->AutogeneratedDefaultValue = Snap.AutogeneratedDefaultValue;

									if (const UEdGraphSchema* Schema = NewPin->GetSchema())
									{
										for (UEdGraphPin* Other : Snap.LinkedTo)
										{
											if (Other)
											{
												Schema->TryCreateConnection(NewPin, Other);
											}
										}
									}
								}
								bAnyChange = true;
							}
							continue;
						}
					}
				}
#endif // WITH_EDITORONLY_DATA

				if (bAnyChange)
				{
					// Optional refresh pass: ReconstructNode + Compile on the
					// external BP. v4 canary showed refresh-only isn't enough in
					// the bundled pass — pre-save state is still inconsistent.
					// Manual workflow (refresh-variables + save-and-compile)
					// works because the compile between refresh and save
					// flushes pin-allocation state. Match that here.
					if (bRefreshAfterRetarget)
					{
						RefreshVarsLikeNodesOnBp(BP, Stats.NodesRefreshed);
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
						FKismetEditorUtilities::CompileBlueprint(BP);
					}
					else
					{
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
					}
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
}

TSharedPtr<FJsonObject> FBlueprintEditOps::VariableLift(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }

	// `vars` is an array of strings (CSV pre-split on the client side).
	const TArray<TSharedPtr<FJsonValue>>* VarsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("vars"), VarsArray) || !VarsArray || VarsArray->Num() == 0)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Missing required param: vars (non-empty array of BP variable names)"));
	}

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	// External-BP retargeting: default ON. A rename step breaks K2Node_Variable
	// refs in other BPs that bound against the pre-rename name; without a scan
	// they orphan silently. `no_scan_external` is the escape hatch for callers
	// who've already verified there are no cross-BP references.
	bool bNoScanExternal = false;
	Params->TryGetBoolField(TEXT("no_scan_external"), bNoScanExternal);

	// Opt-in post-retarget refresh pass. When set, every external BP affected
	// by this lift also gets ReconstructNode'd on its K2Node_Variable / CallFn /
	// Delegate nodes before save. Gamedev's v4 canary showed this is the real
	// unlock for the Target-pin-loss failure mode in UE's compile-time re-
	// resolve; the refs-only retarget isn't always enough. Default OFF to
	// preserve the cheap fast path; set when callers have upcast-dependent
	// external refs (most real migrations).
	bool bRefreshExternalAfterLift = false;
	Params->TryGetBoolField(TEXT("refresh_external_after_lift"), bRefreshExternalAfterLift);

	// Scope for the external scan. Default /Game/ covers the whole project;
	// narrow it for speed on large trees. Only consulted if !bNoScanExternal.
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

	// Build the list of existing BP var names once, for missing-var suggestions.
	TArray<FString> AvailableVars;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		AvailableVars.Add(Var.VarName.ToString());
	}

	TArray<TSharedPtr<FJsonValue>> LiftedJson;
	TArray<TSharedPtr<FJsonValue>> MissingJson;
	int32 Renamed = 0;
	int32 Removed = 0;

	// OldName -> NewName for every var that's actually being renamed. Drives
	// (a) the external ConstructObjectFromClass pin rename and (b) the external
	// K2Node_Variable retarget's name swap.
	TMap<FName, FName> RenameMap;

	// OldName -> FinalName for EVERY lifted var (renames AND same-name removes).
	// Non-rename removes still trigger the external K2Node_Variable pin-link-
	// preservation pass — gamedev's canary on CL 15944 surfaced that UE's own
	// re-resolve narrows the Target pin type and drops the upcast link even
	// without a rename.
	TMap<FName, FName> VarsToProcess;

	for (const TSharedPtr<FJsonValue>& Val : *VarsArray)
	{
		FString Requested = Val->AsString().TrimStartAndEnd();
		if (Requested.IsEmpty()) { continue; }

		const FName CurName(*Requested);
		if (FindVariableIndex(Blueprint, CurName) == INDEX_NONE)
		{
			TSharedPtr<FJsonObject> MissItem = MakeShareable(new FJsonObject);
			MissItem->SetStringField(TEXT("requested"), Requested);
			// v1 suggestion = full available list; callers can fuzzy-match client-side.
			TArray<TSharedPtr<FJsonValue>> Avail;
			for (const FString& AV : AvailableVars) { Avail.Add(MakeShareable(new FJsonValueString(AV))); }
			MissItem->SetArrayField(TEXT("available_vars"), Avail);
			MissingJson.Add(MakeShareable(new FJsonValueObject(MissItem)));
			continue;
		}

		const FString FinalStr = LiftHelpers::ToCppIdentifier(Requested);
		const FName FinalName(*FinalStr);
		const bool bNeedsRename = FinalName != CurName;

		// Collision check: only block if the target already exists AND isn't this var itself.
		if (bNeedsRename && FindVariableIndex(Blueprint, FinalName) != INDEX_NONE)
		{
			TSharedPtr<FJsonObject> MissItem = MakeShareable(new FJsonObject);
			MissItem->SetStringField(TEXT("requested"), Requested);
			MissItem->SetStringField(TEXT("error"), FString::Printf(
				TEXT("Target name '%s' already exists — rename that conflict first, then rerun"),
				*FinalStr));
			MissingJson.Add(MakeShareable(new FJsonValueObject(MissItem)));
			continue;
		}

		TSharedPtr<FJsonObject> LiftItem = MakeShareable(new FJsonObject);
		LiftItem->SetStringField(TEXT("requested"), Requested);
		LiftItem->SetStringField(TEXT("final_name"), FinalStr);
		LiftItem->SetBoolField(TEXT("renamed"), bNeedsRename);

		if (bNeedsRename)
		{
			RenameMap.Add(CurName, FinalName);
		}
		// Record every lifted var (renamed or same-name) so the external
		// retargeter can preserve pin links on both paths.
		VarsToProcess.Add(CurName, FinalName);

		if (!bDryRun)
		{
			if (bNeedsRename)
			{
				FBlueprintEditorUtils::RenameMemberVariable(Blueprint, CurName, FinalName);
				++Renamed;
			}
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FinalName);
			++Removed;
		}
		LiftedJson.Add(MakeShareable(new FJsonValueObject(LiftItem)));
	}

	// External BP retargeting. Needed whenever any var was lifted (rename or
	// same-name remove). Non-rename removes still need a pin-link-preservation
	// pass on external K2Node_Variable refs because UE's re-resolve at external
	// compile narrows the Target pin type and drops the upcast link.
	int32 ExternalNodesRetargeted = 0;
	int32 ExternalNodesRefreshed = 0;
	TArray<TSharedPtr<FJsonValue>> AffectedBpsJson;
	TArray<FString> SaveFailures;
	if (VarsToProcess.Num() > 0 && !bNoScanExternal)
	{
		TArray<UPackage*> PackagesToSave;
		LiftHelpers::FExternalRetargetStats Stats;
		if (bDryRun)
		{
			// Dry-run skips the external scan. Honoring dry-run in the walker
			// would mean either (a) duplicating the walk logic to count without
			// mutating, or (b) mutating in memory then abandoning. Both are
			// worse than a plain "run without --dry-run for real numbers"
			// warning, emitted below via response.warnings.
		}
		else
		{
			Stats = LiftHelpers::RetargetExternalVarRefs(Blueprint, VarsToProcess, RenameMap, ExternalSearchPaths, bRefreshExternalAfterLift, PackagesToSave);
			ExternalNodesRetargeted = Stats.NodesRetargeted;
			ExternalNodesRefreshed = Stats.NodesRefreshed;
			for (const FString& BpPath : Stats.AffectedBpPaths)
			{
				AffectedBpsJson.Add(MakeShareable(new FJsonValueString(BpPath)));
			}

			// Batch save the packages we dirtied. On failure, leave the in-memory
			// retargeting intact (next save/compile will pick it up) and report.
			if (PackagesToSave.Num() > 0)
			{
				const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, /*bOnlyDirty=*/true);
				if (!bSaved)
				{
					SaveFailures.Add(TEXT("One or more retargeted external BP packages failed to save; re-run after checking them out in p4"));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetBoolField(TEXT("dry_run"), bDryRun);
	Response->SetArrayField(TEXT("lifted"), LiftedJson);
	Response->SetArrayField(TEXT("missing"), MissingJson);
	Response->SetNumberField(TEXT("renamed"), Renamed);
	Response->SetNumberField(TEXT("removed"), Removed);
	Response->SetBoolField(TEXT("scanned_external"), !bNoScanExternal && VarsToProcess.Num() > 0);
	Response->SetNumberField(TEXT("external_nodes_retargeted"), ExternalNodesRetargeted);
	Response->SetNumberField(TEXT("external_bps_affected"), AffectedBpsJson.Num());
	Response->SetArrayField(TEXT("external_bps"), AffectedBpsJson);
	Response->SetBoolField(TEXT("refreshed_external_after_lift"), bRefreshExternalAfterLift && !bNoScanExternal && VarsToProcess.Num() > 0);
	Response->SetNumberField(TEXT("external_nodes_refreshed"), ExternalNodesRefreshed);

	// Warnings list — non-fatal conditions the caller should see.
	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (bDryRun && VarsToProcess.Num() > 0 && !bNoScanExternal)
	{
		Warnings.Add(MakeShareable(new FJsonValueString(
			TEXT("dry-run skips external-BP scan; rerun without --dry-run for the actual retargeting + save"))));
	}
	if (bNoScanExternal && VarsToProcess.Num() > 0)
	{
		Warnings.Add(MakeShareable(new FJsonValueString(
			TEXT("no_scan_external is set and at least one var was lifted; external K2Node_Variable refs and K2Node_CreateWidget pins may be orphaned — verify with `digbp findvaruses --var=<OldName>`"))));
	}
	for (const FString& Msg : SaveFailures)
	{
		Warnings.Add(MakeShareable(new FJsonValueString(Msg)));
	}
	if (Warnings.Num() > 0)
	{
		Response->SetArrayField(TEXT("warnings"), Warnings);
	}
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
