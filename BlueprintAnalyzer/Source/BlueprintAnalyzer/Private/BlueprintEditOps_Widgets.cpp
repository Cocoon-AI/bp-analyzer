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
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"

namespace
{
	using FBlueprintEditHelpers::RequireString;

	// Walk a dotted property path (e.g. "Font.Size") starting from a UObject,
	// descending through FStructProperty hops. On success, returns the leaf
	// FProperty + its memory pointer for ImportText. Returns false with a
	// reason for any walk failure (missing prop, non-struct intermediate).
	static bool ResolvePropertyPath(
		UObject* Container,
		const FString& DottedPath,
		FProperty*& OutLeaf,
		void*& OutPtr,
		FString& OutError)
	{
		if (!Container || DottedPath.IsEmpty())
		{
			OutError = TEXT("Empty container or property path");
			return false;
		}

		TArray<FString> Parts;
		DottedPath.ParseIntoArray(Parts, TEXT("."));
		if (Parts.Num() == 0)
		{
			OutError = TEXT("Empty property path after split");
			return false;
		}

		UStruct* CurrentStruct = Container->GetClass();
		void* CurrentPtr = Container;

		for (int32 i = 0; i < Parts.Num(); ++i)
		{
			const bool bIsLeaf = (i == Parts.Num() - 1);
			FProperty* Prop = CurrentStruct->FindPropertyByName(FName(*Parts[i]));
			if (!Prop)
			{
				OutError = FString::Printf(
					TEXT("Property '%s' not found on %s (path so far: %s)"),
					*Parts[i],
					*CurrentStruct->GetName(),
					*FString::Join(Parts.Slice(0, i + 1), TEXT(".")));
				return false;
			}

			if (bIsLeaf)
			{
				OutLeaf = Prop;
				OutPtr = Prop->ContainerPtrToValuePtr<void>(CurrentPtr);
				return true;
			}

			// Intermediate hop must be a struct so we can descend into its
			// inner FProperties (FStructProperty wraps a UScriptStruct).
			FStructProperty* SP = CastField<FStructProperty>(Prop);
			if (!SP)
			{
				OutError = FString::Printf(
					TEXT("Intermediate '%s' is not a struct (was %s); only struct properties support dotted descent"),
					*Parts[i],
					*Prop->GetClass()->GetName());
				return false;
			}
			CurrentStruct = SP->Struct;
			CurrentPtr = SP->ContainerPtrToValuePtr<void>(CurrentPtr);
		}

		// Loop should have returned at the leaf; defensive fallback.
		OutError = TEXT("Property path walk exited without resolving a leaf");
		return false;
	}
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
	FString WalkError;
	if (!ResolvePropertyPath(Target, PropertyName, LeafProp, LeafPtr, WalkError))
	{
		return FBlueprintEditHelpers::MakeEditError(WalkError);
	}

	const TCHAR* ImportResult = LeafProp->ImportText(*Value, LeafPtr, PPF_None, Target);
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
