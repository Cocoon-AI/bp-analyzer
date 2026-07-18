// BlueprintExportFont.cpp
// Composite-font inspection + mutation for UFont assets (runtime-cached).
//
// UFont::CompositeFont is a protected UPROPERTY — editor Python's
// get/set_editor_property refuses it, which is why this lives in native code.
// We reach it via reflection (FindFProperty on UFont::StaticClass()) purely to
// get past the access specifier; the FCompositeFont type itself is public, so
// once we have the container pointer we mutate the struct directly instead of
// round-tripping through ImportText.
//
// Built for l10n fallback wiring: append culture/range-filtered sub-typefaces
// and set the last-resort FallbackTypeface so CJK/Cyrillic text renders in
// project-imported FontFace assets (Noto) instead of engine DroidSansFallback.
//
// Slate resolution rules that shape this API:
// - Sub-typefaces are matched per-codepoint against CharacterRanges (first
//   matching sub-font wins, in array order). A sub-font with NO ranges never
//   matches anything — so add_subfont requires at least one range and points
//   catch-all requests at set_fallback instead.
// - Cultures is a semicolon-separated filter (empty = all cultures).
// - Typeface-entry name matching is loose: if the requested typeface name
//   (e.g. "Black") has no entry in the matched sub-font/fallback, Slate falls
//   back to the best/first entry, so a single "Regular" entry serves all
//   styles.

#include "BlueprintExportCommandlet.h"

#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Fonts/CompositeFont.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FileHelpers.h"

// Unity-build note: free functions in anonymous namespaces collide across .cpp
// files merged into one TU. Use a unique Font_ prefix on locals.

namespace
{
	TSharedPtr<FJsonObject> Font_MakeError(const FString& Message)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		Out->SetBoolField(TEXT("success"), false);
		Out->SetStringField(TEXT("error"), Message);
		return Out;
	}

	UFont* Font_Load(const FString& Path, TSharedPtr<FJsonObject>& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = Font_MakeError(TEXT("Empty path"));
			return nullptr;
		}
		UFont* Font = LoadObject<UFont>(nullptr, *Path);
		if (!Font)
		{
			OutError = Font_MakeError(FString::Printf(TEXT("Failed to load UFont at path: %s"), *Path));
			return nullptr;
		}
		return Font;
	}

	FStructProperty* Font_CompositeFontProperty()
	{
		return FindFProperty<FStructProperty>(UFont::StaticClass(), TEXT("CompositeFont"));
	}

	FCompositeFont* Font_GetCompositeFontMutable(UFont* Font, TSharedPtr<FJsonObject>& OutError)
	{
		FStructProperty* Prop = Font_CompositeFontProperty();
		if (!Prop)
		{
			OutError = Font_MakeError(TEXT("Reflection lookup of UFont::CompositeFont failed (engine layout change?)"));
			return nullptr;
		}
		return Prop->ContainerPtrToValuePtr<FCompositeFont>(Font);
	}

	// Composite fonts only apply to runtime-cached fonts; UFont::GetCompositeFont
	// returns null for Offline and Slate never consults it. Fail loud rather
	// than staging a mutation that can never render.
	bool Font_EnsureRuntimeCached(UFont* Font, TSharedPtr<FJsonObject>& OutError)
	{
		if (Font->FontCacheType != EFontCacheType::Runtime)
		{
			OutError = Font_MakeError(FString::Printf(
				TEXT("Font %s has FontCacheType=Offline; CompositeFont is ignored for offline-cached fonts. Convert the asset to a runtime-cached font first."),
				*Font->GetPathName()));
			return false;
		}
		return true;
	}

	FString Font_MakePersistHint(const FString& Path)
	{
		return FString::Printf(TEXT("digbp font save --path=%s"), *Path);
	}

	// --- Editor-parity commit path (see CLAUDE.md CDO-write guidance) ---
	// Direct struct mutation instead of ImportText, but the same bracketing:
	// Modify/PreEditChange before, PostEditChangeProperty + dirty after.

	void Font_PreChange(UFont* Font, FStructProperty* Prop)
	{
		Font->SetFlags(RF_Transactional);
		Font->Modify();
		Font->PreEditChange(Prop);
	}

	void Font_PostChange(UFont* Font, FStructProperty* Prop)
	{
		FPropertyChangedEvent ChangeEvent(Prop, EPropertyChangeType::ValueSet);
		Font->PostEditChangeProperty(ChangeEvent);
		Font->MarkPackageDirty();
	}

	// --- JSON export helpers ---

	TSharedPtr<FJsonObject> Font_TypefaceEntryToJson(const FTypefaceEntry& Entry)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		Out->SetStringField(TEXT("name"), Entry.Name.ToString());
		if (const UObject* Face = Entry.Font.GetFontFaceAsset())
		{
			Out->SetStringField(TEXT("font_face"), Face->GetPathName());
		}
		else
		{
			// Legacy pre-FontFace-asset entry (raw filename baked into the font).
			Out->SetStringField(TEXT("font_filename"), Entry.Font.GetFontFilename());
		}
		Out->SetStringField(TEXT("hinting"), StaticEnum<EFontHinting>()->GetNameStringByValue((int64)Entry.Font.GetHinting()));
		Out->SetStringField(TEXT("loading_policy"), StaticEnum<EFontLoadingPolicy>()->GetNameStringByValue((int64)Entry.Font.GetLoadingPolicy()));
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> Font_TypefaceToJson(const FTypeface& Typeface)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FTypefaceEntry& Entry : Typeface.Fonts)
		{
			Out.Add(MakeShareable(new FJsonValueObject(Font_TypefaceEntryToJson(Entry))));
		}
		return Out;
	}

	// Normalize a range bound to an inclusive integer; null when open.
	void Font_SetBoundFields(const TSharedPtr<FJsonObject>& Out, const TCHAR* Key, const TCHAR* HexKey, const FInt32RangeBound& Bound, int32 ExclusiveAdjust)
	{
		if (Bound.IsOpen())
		{
			Out->SetField(Key, MakeShareable(new FJsonValueNull()));
			Out->SetField(HexKey, MakeShareable(new FJsonValueNull()));
			return;
		}
		const int32 Value = Bound.IsExclusive() ? Bound.GetValue() + ExclusiveAdjust : Bound.GetValue();
		Out->SetNumberField(Key, Value);
		Out->SetStringField(HexKey, FString::Printf(TEXT("%04X"), Value));
	}

	TSharedPtr<FJsonObject> Font_RangeToJson(const FInt32Range& Range)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		Font_SetBoundFields(Out, TEXT("lower"), TEXT("lower_hex"), Range.GetLowerBound(), +1);
		Font_SetBoundFields(Out, TEXT("upper"), TEXT("upper_hex"), Range.GetUpperBound(), -1);
		return Out;
	}

	TSharedPtr<FJsonObject> Font_SubFontToJson(const FCompositeSubFont& SubFont, int32 Index)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		Out->SetNumberField(TEXT("index"), Index);
#if WITH_EDITORONLY_DATA
		Out->SetStringField(TEXT("editor_name"), SubFont.EditorName.ToString());
#endif
		Out->SetStringField(TEXT("cultures"), SubFont.Cultures);
		Out->SetNumberField(TEXT("scaling_factor"), SubFont.ScalingFactor);
		TArray<TSharedPtr<FJsonValue>> Ranges;
		for (const FInt32Range& Range : SubFont.CharacterRanges)
		{
			Ranges.Add(MakeShareable(new FJsonValueObject(Font_RangeToJson(Range))));
		}
		Out->SetArrayField(TEXT("character_ranges"), Ranges);
		Out->SetArrayField(TEXT("typeface"), Font_TypefaceToJson(SubFont.Typeface));
		return Out;
	}

	TSharedPtr<FJsonObject> Font_MakeSuccess(UFont* Font)
	{
		TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("path"), Font->GetPathName());
		Result->SetStringField(TEXT("name"), Font->GetName());
		Result->SetStringField(TEXT("font_cache_type"), Font->FontCacheType == EFontCacheType::Runtime ? TEXT("Runtime") : TEXT("Offline"));
		return Result;
	}

	// --- Input parsing ---

	// One codepoint in unicode-hex convention ("4E00", "U+4E00", "0x4E00").
	bool Font_ParseHexCodepoint(const FString& RawToken, int32& OutValue, FString& OutError)
	{
		FString Token = RawToken.TrimStartAndEnd();
		Token.RemoveFromStart(TEXT("U+"), ESearchCase::IgnoreCase);
		Token.RemoveFromStart(TEXT("0x"), ESearchCase::IgnoreCase);
		if (Token.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Empty codepoint in range token '%s'"), *RawToken);
			return false;
		}
		for (const TCHAR C : Token)
		{
			if (!FChar::IsHexDigit(C))
			{
				OutError = FString::Printf(TEXT("Invalid hex codepoint '%s' (ranges are hex, e.g. 4E00-9FFF)"), *RawToken);
				return false;
			}
		}
		OutValue = (int32)FParse::HexNumber(*Token);
		if (OutValue > 0x10FFFF)
		{
			OutError = FString::Printf(TEXT("Codepoint '%s' exceeds U+10FFFF"), *RawToken);
			return false;
		}
		return true;
	}

	// "4E00-9FFF,3040-30FF,3005" -> inclusive FInt32Ranges. Hex per unicode
	// convention; a lone codepoint is a single-character range.
	bool Font_ParseRanges(const FString& RangesCsv, TArray<FInt32Range>& OutRanges, FString& OutError)
	{
		TArray<FString> Tokens;
		RangesCsv.ParseIntoArray(Tokens, TEXT(","), /*bCullEmpty=*/true);
		for (const FString& Token : Tokens)
		{
			const FString Trimmed = Token.TrimStartAndEnd();
			FString LoStr, HiStr;
			if (!Trimmed.Split(TEXT("-"), &LoStr, &HiStr))
			{
				LoStr = Trimmed;
				HiStr = Trimmed;
			}
			int32 Lo = 0, Hi = 0;
			if (!Font_ParseHexCodepoint(LoStr, Lo, OutError)) { return false; }
			if (!Font_ParseHexCodepoint(HiStr, Hi, OutError)) { return false; }
			if (Lo > Hi)
			{
				OutError = FString::Printf(TEXT("Range lower > upper in '%s'"), *Trimmed);
				return false;
			}
			OutRanges.Add(FInt32Range(FInt32RangeBound::Inclusive(Lo), FInt32RangeBound::Inclusive(Hi)));
		}
		if (OutRanges.Num() == 0)
		{
			OutError = TEXT("No character ranges parsed");
			return false;
		}
		return true;
	}

	// Engine matching splits Cultures on ';'. Accept commas from the CLI and
	// normalize, trimming whitespace per entry.
	FString Font_NormalizeCultures(const FString& In)
	{
		FString Working = In;
		Working.ReplaceInline(TEXT(","), TEXT(";"));
		TArray<FString> Parts;
		Working.ParseIntoArray(Parts, TEXT(";"), /*bCullEmpty=*/true);
		for (FString& Part : Parts) { Part.TrimStartAndEndInline(); }
		return FString::Join(Parts, TEXT(";"));
	}

	UFontFace* Font_LoadFace(const FString& FontFacePath, TSharedPtr<FJsonObject>& OutError)
	{
		if (FontFacePath.IsEmpty())
		{
			OutError = Font_MakeError(TEXT("Empty font_face path"));
			return nullptr;
		}
		UObject* FaceObj = LoadObject<UObject>(nullptr, *FontFacePath);
		if (!FaceObj)
		{
			OutError = Font_MakeError(FString::Printf(TEXT("Failed to load FontFace asset at path: %s"), *FontFacePath));
			return nullptr;
		}
		UFontFace* Face = Cast<UFontFace>(FaceObj);
		if (!Face)
		{
			OutError = Font_MakeError(FString::Printf(TEXT("Asset is not a UFontFace (got %s): %s"), *FaceObj->GetClass()->GetName(), *FontFacePath));
			return nullptr;
		}
		return Face;
	}

	FTypefaceEntry Font_MakeTypefaceEntry(const FString& TypefaceName, UFontFace* Face)
	{
		FTypefaceEntry Entry;
		Entry.Name = FName(*TypefaceName);
		// FFontData(face) picks up hinting/loading policy from the face asset.
		Entry.Font = FFontData(Face);
		return Entry;
	}
}

//------------------------------------------------------------------------------
// font.export — dump CompositeFont as JSON
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::FontExportToJson(const FString& Path)
{
	TSharedPtr<FJsonObject> Err;
	UFont* Font = Font_Load(Path, Err);
	if (!Font) { return Err; }

	FCompositeFont* Composite = Font_GetCompositeFontMutable(Font, Err);
	if (!Composite) { return Err; }

	TSharedPtr<FJsonObject> Result = Font_MakeSuccess(Font);

	TSharedPtr<FJsonObject> CompositeObj = MakeShareable(new FJsonObject);
	CompositeObj->SetArrayField(TEXT("default_typeface"), Font_TypefaceToJson(Composite->DefaultTypeface));

	TSharedPtr<FJsonObject> FallbackObj = MakeShareable(new FJsonObject);
	FallbackObj->SetNumberField(TEXT("scaling_factor"), Composite->FallbackTypeface.ScalingFactor);
	FallbackObj->SetArrayField(TEXT("typeface"), Font_TypefaceToJson(Composite->FallbackTypeface.Typeface));
	CompositeObj->SetObjectField(TEXT("fallback"), FallbackObj);

	TArray<TSharedPtr<FJsonValue>> SubFonts;
	for (int32 Index = 0; Index < Composite->SubTypefaces.Num(); ++Index)
	{
		SubFonts.Add(MakeShareable(new FJsonValueObject(Font_SubFontToJson(Composite->SubTypefaces[Index], Index))));
	}
	CompositeObj->SetArrayField(TEXT("sub_typefaces"), SubFonts);

	Result->SetObjectField(TEXT("composite_font"), CompositeObj);
	return Result;
}

//------------------------------------------------------------------------------
// font.add_subfont — append a culture/range-filtered sub-typeface
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::FontAddSubfontToJson(const FString& Path, const FString& FontFacePath, const FString& Cultures, const FString& Ranges, const FString& TypefaceName, const FString& EditorName, double ScalingFactor)
{
	TSharedPtr<FJsonObject> Err;
	UFont* Font = Font_Load(Path, Err);
	if (!Font) { return Err; }
	if (!Font_EnsureRuntimeCached(Font, Err)) { return Err; }

	UFontFace* Face = Font_LoadFace(FontFacePath, Err);
	if (!Face) { return Err; }

	FCompositeFont* Composite = Font_GetCompositeFontMutable(Font, Err);
	if (!Composite) { return Err; }

	// A sub-font with no CharacterRanges never matches any codepoint — Slate
	// only consults sub-typefaces through their range table. Refuse instead of
	// silently staging a no-op; catch-all is what set_fallback is for.
	if (Ranges.TrimStartAndEnd().IsEmpty())
	{
		return Font_MakeError(TEXT("At least one character range is required (a sub-typeface with no ranges never matches; use font.set_fallback for a catch-all)"));
	}
	TArray<FInt32Range> ParsedRanges;
	FString ParseError;
	if (!Font_ParseRanges(Ranges, ParsedRanges, ParseError))
	{
		return Font_MakeError(ParseError);
	}

	FStructProperty* Prop = Font_CompositeFontProperty();
	Font_PreChange(Font, Prop);

	FCompositeSubFont NewSub;
	NewSub.Cultures = Font_NormalizeCultures(Cultures);
	NewSub.CharacterRanges = MoveTemp(ParsedRanges);
	NewSub.ScalingFactor = (float)ScalingFactor;
#if WITH_EDITORONLY_DATA
	NewSub.EditorName = FName(EditorName.IsEmpty() ? *Face->GetName() : *EditorName);
#endif
	NewSub.Typeface.Fonts.Add(Font_MakeTypefaceEntry(TypefaceName, Face));

	const int32 NewIndex = Composite->SubTypefaces.Add(MoveTemp(NewSub));

	Font_PostChange(Font, Prop);

	TSharedPtr<FJsonObject> Result = Font_MakeSuccess(Font);
	Result->SetNumberField(TEXT("index"), NewIndex);
	Result->SetObjectField(TEXT("sub_typeface"), Font_SubFontToJson(Composite->SubTypefaces[NewIndex], NewIndex));
	Result->SetBoolField(TEXT("dirty"), true);
	Result->SetBoolField(TEXT("staged"), true);
	Result->SetStringField(TEXT("persist_with"), Font_MakePersistHint(Path));
	return Result;
}

//------------------------------------------------------------------------------
// font.set_fallback — set/replace the last-resort fallback typeface
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::FontSetFallbackToJson(const FString& Path, const FString& FontFacePath, const FString& TypefaceName, double ScalingFactor)
{
	TSharedPtr<FJsonObject> Err;
	UFont* Font = Font_Load(Path, Err);
	if (!Font) { return Err; }
	if (!Font_EnsureRuntimeCached(Font, Err)) { return Err; }

	UFontFace* Face = Font_LoadFace(FontFacePath, Err);
	if (!Face) { return Err; }

	FCompositeFont* Composite = Font_GetCompositeFontMutable(Font, Err);
	if (!Composite) { return Err; }

	// Capture what we're replacing so the caller can reconstruct it if needed.
	const TArray<TSharedPtr<FJsonValue>> PreviousTypeface = Font_TypefaceToJson(Composite->FallbackTypeface.Typeface);

	FStructProperty* Prop = Font_CompositeFontProperty();
	Font_PreChange(Font, Prop);

	Composite->FallbackTypeface.Typeface.Fonts.Reset();
	Composite->FallbackTypeface.Typeface.Fonts.Add(Font_MakeTypefaceEntry(TypefaceName, Face));
	Composite->FallbackTypeface.ScalingFactor = (float)ScalingFactor;

	Font_PostChange(Font, Prop);

	TSharedPtr<FJsonObject> Result = Font_MakeSuccess(Font);
	TSharedPtr<FJsonObject> FallbackObj = MakeShareable(new FJsonObject);
	FallbackObj->SetNumberField(TEXT("scaling_factor"), Composite->FallbackTypeface.ScalingFactor);
	FallbackObj->SetArrayField(TEXT("typeface"), Font_TypefaceToJson(Composite->FallbackTypeface.Typeface));
	Result->SetObjectField(TEXT("fallback"), FallbackObj);
	Result->SetArrayField(TEXT("previous_typeface"), PreviousTypeface);
	Result->SetBoolField(TEXT("dirty"), true);
	Result->SetBoolField(TEXT("staged"), true);
	Result->SetStringField(TEXT("persist_with"), Font_MakePersistHint(Path));
	return Result;
}

//------------------------------------------------------------------------------
// font.remove_subfont — remove a sub-typeface by index (from font.export)
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::FontRemoveSubfontToJson(const FString& Path, int32 Index)
{
	TSharedPtr<FJsonObject> Err;
	UFont* Font = Font_Load(Path, Err);
	if (!Font) { return Err; }

	FCompositeFont* Composite = Font_GetCompositeFontMutable(Font, Err);
	if (!Composite) { return Err; }

	if (!Composite->SubTypefaces.IsValidIndex(Index))
	{
		return Font_MakeError(FString::Printf(TEXT("Sub-typeface index %d out of range (font has %d)"), Index, Composite->SubTypefaces.Num()));
	}

	const TSharedPtr<FJsonObject> RemovedJson = Font_SubFontToJson(Composite->SubTypefaces[Index], Index);

	FStructProperty* Prop = Font_CompositeFontProperty();
	Font_PreChange(Font, Prop);
	Composite->SubTypefaces.RemoveAt(Index);
	Font_PostChange(Font, Prop);

	TSharedPtr<FJsonObject> Result = Font_MakeSuccess(Font);
	Result->SetObjectField(TEXT("removed"), RemovedJson);
	Result->SetNumberField(TEXT("remaining_sub_typefaces"), Composite->SubTypefaces.Num());
	Result->SetBoolField(TEXT("dirty"), true);
	Result->SetBoolField(TEXT("staged"), true);
	Result->SetStringField(TEXT("persist_with"), Font_MakePersistHint(Path));
	return Result;
}

//------------------------------------------------------------------------------
// font.save — persist package (generic asset save; fonts are not Blueprints)
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::FontSaveToJson(const FString& Path)
{
	TSharedPtr<FJsonObject> Err;
	UFont* Font = Font_Load(Path, Err);
	if (!Font) { return Err; }

	UPackage* Package = Font->GetOutermost();
	if (!Package)
	{
		return Font_MakeError(TEXT("Font has no outer package"));
	}

	TSharedPtr<FJsonObject> Result = Font_MakeSuccess(Font);
	if (!Package->IsDirty())
	{
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("not_dirty"), true);
		return Result;
	}

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, /*bOnlyDirty=*/true);
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (!bSaved)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("SavePackages reported failure"));
	}
	return Result;
}
