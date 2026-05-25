// BlueprintEditOps_Widgets.cpp
// UMG WidgetTree edit operations. Distinct from BlueprintEditOps_Components
// (which handles SimpleConstructionScript actor components) — UMG widgets live
// in UWidgetBlueprint::WidgetTree and are addressed by name via
// UWidgetTree::FindWidget rather than by SCS node lookup.

#include "BlueprintEditOps.h"
#include "BlueprintEditHelpers.h"

#include "Engine/Blueprint.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetNavigation.h"
#include "Animation/WidgetAnimation.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "K2Node_Variable.h"
#include "EdGraph/EdGraph.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"

namespace
{
	using FBlueprintEditHelpers::RequireString;
}

//------------------------------------------------------------------------------
// edit.widget.set_property
//
// Set a property on a UMG widget archetype within a WidgetBlueprint's
// WidgetTree. Mirrors edit.component.set_property but targets the UMG tree
// instead of SCS.
//
// Property is a dotted path: "Text", "Font.Size", "Font.FontObject",
// "ColorAndOpacity". The walk recurses through FStructProperty intermediates
// so callers can target individual struct fields without having to format
// the entire struct's UE-text.
//
// Value is UE-text format (same as cdo set / component set-property):
//   bool:   true / false
//   number: 24
//   FName:  "MyName"
//   FText:  NSLOCTEXT(...) or quoted string
//   FLinearColor: (R=...,G=...,B=...,A=...)
//   object: /Game/Path/Asset.Asset (path)
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::WidgetSetProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, WidgetName, PropertyName, Value;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("widget"), WidgetName, Err)) { return Err; }
	if (!RequireString(Params, TEXT("property"), PropertyName, Err)) { return Err; }
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("Missing required param: value"));
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Blueprint);
	if (!WidgetBP)
	{
		return FBlueprintEditHelpers::MakeEditError(
			FString::Printf(TEXT("Blueprint '%s' is not a WidgetBlueprint"), *Path));
	}
	if (!WidgetBP->WidgetTree)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("WidgetBlueprint has no WidgetTree"));
	}

	UWidget* Target = WidgetBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Target)
	{
		return FBlueprintEditHelpers::MakeEditError(
			FString::Printf(TEXT("Widget '%s' not found in WidgetTree"), *WidgetName));
	}

	FProperty* LeafProp = nullptr;
	void* LeafPtr = nullptr;
	UObject* InnermostOwner = nullptr;
	FString WalkError;
	if (!FBlueprintEditHelpers::ResolvePropertyPath(Target, PropertyName, LeafProp, LeafPtr, InnermostOwner, WalkError))
	{
		return FBlueprintEditHelpers::MakeEditError(WalkError);
	}

	const TCHAR* ImportResult = LeafProp->ImportText(*Value, LeafPtr, PPF_None, InnermostOwner);
	if (ImportResult == nullptr)
	{
		return FBlueprintEditHelpers::MakeEditError(
			FString::Printf(TEXT("ImportText failed for value '%s' on property %s"), *Value, *PropertyName));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path);
	Response->SetStringField(TEXT("widget"), WidgetName);
	Response->SetStringField(TEXT("widget_class"), Target->GetClass()->GetName());
	Response->SetStringField(TEXT("property"), PropertyName);
	Response->SetStringField(TEXT("value"), Value);
	return Response;
}

//------------------------------------------------------------------------------
// edit.widget.rename
//
// Rename a UMG widget in a WidgetBlueprint's WidgetTree, retargeting every
// internal reference. Headless port of UMGEditor's
// FWidgetBlueprintEditorUtils::RenameWidget — same steps minus the editor-only
// preview-widget rename (there is no live preview in a commandlet/server) and
// the cosmetic MovieScene possessable label (the functional animation binding is
// retargeted via FWidgetAnimationBinding::WidgetName, which is GUID-bound, so the
// track keeps working; only its display label in the anim editor stays old).
//
// Retargets:
//   - the widget UObject's FName + display label
//   - the auto-generated UWidget* member variable (regenerated on recompile)
//   - K2Node_VariableGet/Set referencing the widget (ReplaceVariableReferences)
//   - K2Node_ComponentBoundEvent bindings — handled inside ReplaceVariableReferences
//     via UK2Node_ComponentBoundEvent::HandleVariableRenamed
//   - property/event delegate bindings, widget-animation bindings, navigation bindings
//
// BindWidget bypass: if the parent class already declares a meta=(BindWidget)
// UWidget* property matching the new name (and the widget's class is compatible),
// the usual name-collision check is bypassed. This is exactly the
// rename-to-match-a-C++-BindWidget case (reparenting BP widgets onto a C++ base
// with BindWidget UPROPERTYs).
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintEditOps::WidgetRename(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, OldName, NewName;
	TSharedPtr<FJsonObject> Err;
	if (!RequireString(Params, TEXT("path"), Path, Err)) { return Err; }
	if (!RequireString(Params, TEXT("old_name"), OldName, Err)) { return Err; }
	if (!RequireString(Params, TEXT("new_name"), NewName, Err)) { return Err; }

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintEditHelpers::LoadBlueprintForEdit(Path, LoadError);
	if (!Blueprint) { return FBlueprintEditHelpers::MakeEditError(LoadError); }

	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Blueprint);
	if (!WidgetBP)
	{
		return FBlueprintEditHelpers::MakeEditError(
			FString::Printf(TEXT("Blueprint '%s' is not a WidgetBlueprint"), *Path));
	}
	if (!WidgetBP->WidgetTree)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("WidgetBlueprint has no WidgetTree"));
	}

	const FName OldFName(*OldName);
	UWidget* Widget = WidgetBP->WidgetTree->FindWidget(OldFName);
	if (!Widget)
	{
		return FBlueprintEditHelpers::MakeEditError(
			FString::Printf(TEXT("Widget '%s' not found in WidgetTree"), *OldName));
	}

	UClass* ParentClass = WidgetBP->ParentClass;
	if (!ParentClass)
	{
		return FBlueprintEditHelpers::MakeEditError(TEXT("WidgetBlueprint has no parent class"));
	}

	// Derive the FName slug from the requested display name (mirrors the editor:
	// spaces and illegal characters get sanitized into the actual object name).
	const FName NewFName = MakeObjectNameFromDisplayLabel(NewName, Widget->GetFName());
	const FString NewNameStr = NewFName.ToString();

	if (NewFName == OldFName)
	{
		return FBlueprintEditHelpers::MakeEditError(
			FString::Printf(TEXT("New name resolves to the same FName '%s' — nothing to rename"), *NewNameStr));
	}

	// BindWidget bypass: allow renaming onto an existing parent-class property when
	// that property is a meta=(BindWidget) of a compatible widget class. (Mirror of
	// FWidgetBlueprintEditorUtils::IsBindWidgetProperty, reimplemented locally to
	// avoid depending on UMGEditor's private header.)
	FObjectPropertyBase* ExistingProperty = CastField<FObjectPropertyBase>(ParentClass->FindPropertyByName(NewFName));
	const bool bBindWidget =
		ExistingProperty &&
		(ExistingProperty->HasMetaData(TEXT("BindWidget")) || ExistingProperty->HasMetaData(TEXT("BindWidgetOptional"))) &&
		Widget->IsA(ExistingProperty->PropertyClass);

	TSharedPtr<INameValidatorInterface> NameValidator = MakeShareable(new FKismetNameValidator(Blueprint));
	const EValidatorResult ValidationResult = NameValidator->IsValid(NewFName);
	if (ValidationResult != EValidatorResult::Ok && !bBindWidget)
	{
		return FBlueprintEditHelpers::MakeEditError(
			FString::Printf(TEXT("New name '%s' is not valid: %s"),
				*NewNameStr,
				*INameValidatorInterface::GetErrorString(NewNameStr, ValidationResult)));
	}

	TArray<FString> Warnings;
	if (!Widget->bIsVariable)
	{
		Warnings.Add(TEXT("Widget is not marked 'Is Variable' — no member variable is generated, so a C++ meta=(BindWidget) will not bind it. Mark it as a variable in the UMG editor if you intend to bind it."));
	}

	Blueprint->Modify();
	Widget->Modify();

	// Rename the template widget (no live preview to mirror in headless mode).
	Widget->SetDisplayLabel(NewName);
	Widget->Rename(*NewNameStr);

	// Give any stale getters/setters orphan pins so wrong-typed connections invalidate
	// cleanly (matches the editor's pre-ReplaceVariableReferences pass; a no-op for a
	// straightforward rename where nothing yet references the new name).
	if (Widget->bIsVariable)
	{
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (const UEdGraph* CurrentGraph : AllGraphs)
		{
			TArray<UK2Node_Variable*> GraphNodes;
			CurrentGraph->GetNodesOfClass(GraphNodes);
			for (UK2Node_Variable* CurrentNode : GraphNodes)
			{
				UClass* SelfClass = Blueprint->GeneratedClass;
				UClass* VariableParent = CurrentNode->VariableReference.GetMemberParentClass(SelfClass);
				if (SelfClass == VariableParent && NewFName == CurrentNode->GetVarName())
				{
					if (UEdGraphPin* ValuePin = CurrentNode->GetValuePin())
					{
						ValuePin->Modify();
						CurrentNode->Modify();
						CurrentNode->CreatePin(
							ValuePin->Direction,
							ValuePin->PinType.PinCategory,
							ValuePin->PinType.PinSubCategory,
							Widget->WidgetGeneratedByClass.Get(),
							NewFName);
						ValuePin->bOrphanedPin = true;
					}
				}
			}
		}
	}

	// Retarget VariableGet/Set references and (via the per-node HandleVariableRenamed
	// hook this call invokes) K2Node_ComponentBoundEvent component bindings.
	FBlueprintEditorUtils::ReplaceVariableReferences(Blueprint, OldFName, NewFName);

	// Property/event delegate bindings (UWidgetBlueprint::Bindings keys by object name string).
	int32 DelegateBindingsUpdated = 0;
	for (FDelegateEditorBinding& Binding : WidgetBP->Bindings)
	{
		if (Binding.ObjectName == OldName)
		{
			Binding.ObjectName = NewNameStr;
			++DelegateBindingsUpdated;
		}
	}

	// Widget-animation bindings (functional FName rebind; possessable GUID is
	// preserved, so only the anim-editor track label stays as the old name).
	int32 AnimBindingsUpdated = 0;
	for (UWidgetAnimation* WidgetAnimation : WidgetBP->Animations)
	{
		if (!WidgetAnimation) { continue; }
		for (FWidgetAnimationBinding& AnimBinding : WidgetAnimation->AnimationBindings)
		{
			if (AnimBinding.WidgetName == OldFName)
			{
				AnimBinding.WidgetName = NewFName;
				++AnimBindingsUpdated;
			}
		}
	}

	// Navigation bindings that reference the widget by name.
	WidgetBP->WidgetTree->ForEachWidget([OldFName, NewFName](UWidget* W)
	{
		if (W && W->Navigation)
		{
			W->Navigation->SetFlags(RF_Transactional);
			W->Navigation->Modify();
			W->Navigation->TryToRenameBinding(OldFName, NewFName);
		}
	});

	// Guard against child-BP variable collisions, then structurally recompile so
	// the generated UWidget* member variable is regenerated under the new name.
	FBlueprintEditorUtils::ValidateBlueprintChildVariables(Blueprint, NewFName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Response = FBlueprintEditHelpers::MakeEditSuccess(Path, Warnings);
	Response->SetStringField(TEXT("old_name"), OldName);
	Response->SetStringField(TEXT("new_name"), NewNameStr);
	Response->SetStringField(TEXT("widget_class"), Widget->GetClass()->GetName());
	Response->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
	Response->SetBoolField(TEXT("bind_widget_match"), bBindWidget);
	Response->SetNumberField(TEXT("delegate_bindings_updated"), DelegateBindingsUpdated);
	Response->SetNumberField(TEXT("animation_bindings_updated"), AnimBindingsUpdated);
	return Response;
}
