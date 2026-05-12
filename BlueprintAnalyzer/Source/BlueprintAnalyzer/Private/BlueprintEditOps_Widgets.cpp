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
