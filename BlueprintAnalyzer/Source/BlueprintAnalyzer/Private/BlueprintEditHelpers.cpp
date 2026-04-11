// BlueprintEditHelpers.cpp
// Implementation of shared edit utilities. Phase A: blueprint loading, pin type
// parsing, response builders. Phase D will append graph/node/pin lookups.

#include "BlueprintEditHelpers.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace FBlueprintEditHelpers
{

//------------------------------------------------------------------------------
// LoadBlueprintForEdit
//------------------------------------------------------------------------------

UBlueprint* LoadBlueprintForEdit(const FString& BlueprintPath, FString& OutError)
{
	if (BlueprintPath.IsEmpty())
	{
		OutError = TEXT("Empty blueprint path");
		return nullptr;
	}

	// Primary path: direct load of the UBlueprint asset.
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (Blueprint)
	{
		return Blueprint;
	}

	// Fallback: caller passed a class path like /Game/Foo/BP_Bar.BP_Bar_C.
	// Try appending _C, load as class, then walk back to the generating blueprint.
	FString ClassPath = BlueprintPath;
	if (!ClassPath.EndsWith(TEXT("_C")))
	{
		ClassPath += TEXT("_C");
	}

	if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath))
	{
		Blueprint = Cast<UBlueprint>(LoadedClass->ClassGeneratedBy);
		if (Blueprint)
		{
			return Blueprint;
		}
	}

	OutError = FString::Printf(TEXT("Failed to load blueprint at path: %s"), *BlueprintPath);
	return nullptr;
}

//------------------------------------------------------------------------------
// ParsePinType helpers
//------------------------------------------------------------------------------

namespace
{
	// Strip outer whitespace.
	static FString Trim(const FString& In)
	{
		FString Out = In;
		Out.TrimStartAndEndInline();
		return Out;
	}

	// If Str is "Wrapper<Inner>", return Inner and set bOk=true. Otherwise bOk=false.
	// Handles nested angle brackets correctly by depth-counting.
	static FString ExtractGenericInner(const FString& Str, const FString& Wrapper, bool& bOk)
	{
		bOk = false;
		const FString Prefix = Wrapper + TEXT("<");
		if (!Str.StartsWith(Prefix) || !Str.EndsWith(TEXT(">")))
		{
			return FString();
		}

		const int32 InnerStart = Prefix.Len();
		const int32 InnerEnd = Str.Len() - 1;
		if (InnerEnd <= InnerStart)
		{
			return FString();
		}

		bOk = true;
		return Str.Mid(InnerStart, InnerEnd - InnerStart);
	}

	// Split "K,V" at the top-level comma (respecting nested angle brackets).
	static bool SplitMapArgs(const FString& Inner, FString& OutKey, FString& OutValue)
	{
		int32 Depth = 0;
		for (int32 i = 0; i < Inner.Len(); ++i)
		{
			const TCHAR C = Inner[i];
			if (C == TEXT('<')) { ++Depth; }
			else if (C == TEXT('>')) { --Depth; }
			else if (C == TEXT(',') && Depth == 0)
			{
				OutKey = Trim(Inner.Left(i));
				OutValue = Trim(Inner.Mid(i + 1));
				return true;
			}
		}
		return false;
	}

	// Look up a UScriptStruct by short name or path. Returns nullptr if not found.
	static UScriptStruct* FindStructByName(const FString& Name)
	{
		if (UScriptStruct* Found = FindObject<UScriptStruct>(ANY_PACKAGE, *Name))
		{
			return Found;
		}
		if (UScriptStruct* Loaded = LoadObject<UScriptStruct>(nullptr, *Name))
		{
			return Loaded;
		}
		// Try with an F prefix (FVector, FRotator, ...)
		if (!Name.StartsWith(TEXT("F")))
		{
			const FString Prefixed = FString(TEXT("F")) + Name;
			if (UScriptStruct* Found = FindObject<UScriptStruct>(ANY_PACKAGE, *Prefixed))
			{
				return Found;
			}
		}
		return nullptr;
	}

	// Look up a UClass by short name or path.
	static UClass* FindClassByName(const FString& Name)
	{
		if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *Name))
		{
			return Found;
		}
		if (UClass* Loaded = LoadObject<UClass>(nullptr, *Name))
		{
			return Loaded;
		}
		// Try common prefixes: U, A
		for (const TCHAR* Prefix : { TEXT("U"), TEXT("A") })
		{
			if (!Name.StartsWith(Prefix))
			{
				const FString Prefixed = FString(Prefix) + Name;
				if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *Prefixed))
				{
					return Found;
				}
			}
		}
		return nullptr;
	}

	// Fill a pin type for a convenience struct alias (vector, rotator, ...). Returns
	// false if the alias is not recognized.
	static bool TrySetConvenienceStruct(const FString& AliasLower, FEdGraphPinType& OutPinType)
	{
		struct FAlias { const TCHAR* Name; const TCHAR* StructName; };
		static const FAlias Aliases[] = {
			{ TEXT("vector"),      TEXT("Vector") },
			{ TEXT("vector2d"),    TEXT("Vector2D") },
			{ TEXT("vector4"),     TEXT("Vector4") },
			{ TEXT("rotator"),     TEXT("Rotator") },
			{ TEXT("transform"),   TEXT("Transform") },
			{ TEXT("color"),       TEXT("Color") },
			{ TEXT("linearcolor"), TEXT("LinearColor") },
			{ TEXT("quat"),        TEXT("Quat") },
			{ TEXT("guid"),        TEXT("Guid") },
			{ TEXT("datetime"),    TEXT("DateTime") },
			{ TEXT("timespan"),    TEXT("Timespan") },
		};

		for (const FAlias& A : Aliases)
		{
			if (AliasLower == A.Name)
			{
				if (UScriptStruct* Struct = FindStructByName(A.StructName))
				{
					OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
					OutPinType.PinSubCategoryObject = Struct;
					return true;
				}
			}
		}
		return false;
	}

	// Parse a single (non-container) type token into the pin type fields.
	// Recursive calls handled at a higher layer.
	static bool ParseSimpleType(const FString& TypeStr, FEdGraphPinType& OutPinType, FString& OutError)
	{
		const FString Trimmed = Trim(TypeStr);
		if (Trimmed.IsEmpty())
		{
			OutError = TEXT("Empty type");
			return false;
		}

		// object<ClassName>
		{
			bool bOk = false;
			const FString Inner = ExtractGenericInner(Trimmed, TEXT("object"), bOk);
			if (bOk)
			{
				UClass* Class = FindClassByName(Trim(Inner));
				if (!Class)
				{
					OutError = FString::Printf(TEXT("Unknown class for object<>: %s"), *Inner);
					return false;
				}
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				OutPinType.PinSubCategoryObject = Class;
				return true;
			}
		}

		// class<ClassName>
		{
			bool bOk = false;
			const FString Inner = ExtractGenericInner(Trimmed, TEXT("class"), bOk);
			if (bOk)
			{
				UClass* Class = FindClassByName(Trim(Inner));
				if (!Class)
				{
					OutError = FString::Printf(TEXT("Unknown class for class<>: %s"), *Inner);
					return false;
				}
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Class;
				OutPinType.PinSubCategoryObject = Class;
				return true;
			}
		}

		// struct<StructName>
		{
			bool bOk = false;
			const FString Inner = ExtractGenericInner(Trimmed, TEXT("struct"), bOk);
			if (bOk)
			{
				UScriptStruct* Struct = FindStructByName(Trim(Inner));
				if (!Struct)
				{
					OutError = FString::Printf(TEXT("Unknown struct for struct<>: %s"), *Inner);
					return false;
				}
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutPinType.PinSubCategoryObject = Struct;
				return true;
			}
		}

		// Primitives (case-insensitive)
		const FString Lower = Trimmed.ToLower();
		if (Lower == TEXT("bool"))            { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean; return true; }
		if (Lower == TEXT("byte"))            { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte; return true; }
		if (Lower == TEXT("int") || Lower == TEXT("integer")) { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int; return true; }
		if (Lower == TEXT("int64"))           { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64; return true; }
		if (Lower == TEXT("float") || Lower == TEXT("real")) { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Float; return true; }
		if (Lower == TEXT("string"))          { OutPinType.PinCategory = UEdGraphSchema_K2::PC_String; return true; }
		if (Lower == TEXT("name"))            { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name; return true; }
		if (Lower == TEXT("text"))            { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text; return true; }

		// Convenience struct aliases
		if (TrySetConvenienceStruct(Lower, OutPinType))
		{
			return true;
		}

		OutError = FString::Printf(TEXT("Unrecognized type: '%s'"), *Trimmed);
		return false;
	}
}

bool ParsePinType(const FString& TypeString, FEdGraphPinType& OutPinType, FString& OutError)
{
	OutPinType = FEdGraphPinType();
	OutPinType.ContainerType = EPinContainerType::None;

	const FString Trimmed = Trim(TypeString);
	if (Trimmed.IsEmpty())
	{
		OutError = TEXT("Empty type string");
		return false;
	}

	// TArray<Inner>
	{
		bool bOk = false;
		const FString Inner = ExtractGenericInner(Trimmed, TEXT("TArray"), bOk);
		if (bOk)
		{
			if (!ParseSimpleType(Inner, OutPinType, OutError))
			{
				return false;
			}
			OutPinType.ContainerType = EPinContainerType::Array;
			return true;
		}
	}

	// TSet<Inner>
	{
		bool bOk = false;
		const FString Inner = ExtractGenericInner(Trimmed, TEXT("TSet"), bOk);
		if (bOk)
		{
			if (!ParseSimpleType(Inner, OutPinType, OutError))
			{
				return false;
			}
			OutPinType.ContainerType = EPinContainerType::Set;
			return true;
		}
	}

	// TMap<K,V>
	{
		bool bOk = false;
		const FString Inner = ExtractGenericInner(Trimmed, TEXT("TMap"), bOk);
		if (bOk)
		{
			FString KeyStr, ValueStr;
			if (!SplitMapArgs(Inner, KeyStr, ValueStr))
			{
				OutError = FString::Printf(TEXT("TMap<> requires two comma-separated type arguments, got: %s"), *Inner);
				return false;
			}

			// Key type goes on the main pin type.
			if (!ParseSimpleType(KeyStr, OutPinType, OutError))
			{
				return false;
			}
			OutPinType.ContainerType = EPinContainerType::Map;

			// Value type goes in PinValueType.
			FEdGraphPinType ValueType;
			if (!ParseSimpleType(ValueStr, ValueType, OutError))
			{
				return false;
			}
			OutPinType.PinValueType.TerminalCategory = ValueType.PinCategory;
			OutPinType.PinValueType.TerminalSubCategory = ValueType.PinSubCategory;
			OutPinType.PinValueType.TerminalSubCategoryObject = ValueType.PinSubCategoryObject;
			return true;
		}
	}

	// Non-container: delegate to simple parser.
	return ParseSimpleType(Trimmed, OutPinType, OutError);
}

//------------------------------------------------------------------------------
// Shared param/context helpers (consolidated here to survive UBT unity builds,
// which merge multiple .cpp files into one TU and break per-file
// anonymous-namespace helpers)
//------------------------------------------------------------------------------

bool RequireString(const TSharedPtr<FJsonObject>& Params, const FString& Field, FString& OutValue, TSharedPtr<FJsonObject>& OutError)
{
	if (!Params.IsValid() || !Params->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
	{
		OutError = MakeEditError(FString::Printf(TEXT("Missing required param: %s"), *Field));
		return false;
	}
	return true;
}

bool ResolveBPAndGraph(const TSharedPtr<FJsonObject>& Params, FBPGraphContext& Out, TSharedPtr<FJsonObject>& OutError)
{
	FString Path, GraphName;
	if (!RequireString(Params, TEXT("path"), Path, OutError)) { return false; }
	if (!RequireString(Params, TEXT("graph"), GraphName, OutError)) { return false; }

	FString LoadError;
	Out.Blueprint = LoadBlueprintForEdit(Path, LoadError);
	if (!Out.Blueprint)
	{
		OutError = MakeEditError(LoadError);
		return false;
	}
	Out.Graph = FindGraphByName(Out.Blueprint, GraphName, LoadError);
	if (!Out.Graph)
	{
		OutError = MakeEditError(LoadError);
		return false;
	}
	return true;
}

FString BPPath(UBlueprint* Blueprint)
{
	return Blueprint ? Blueprint->GetPathName() : FString();
}

//------------------------------------------------------------------------------
// Response builders
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> MakeEditSuccess(const FString& BlueprintPath, const TArray<FString>& Warnings)
{
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("path"), BlueprintPath);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningArray;
		for (const FString& W : Warnings)
		{
			WarningArray.Add(MakeShareable(new FJsonValueString(W)));
		}
		Result->SetArrayField(TEXT("warnings"), WarningArray);
	}

	return Result;
}

TSharedPtr<FJsonObject> MakeEditError(const FString& Error)
{
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), false);
	Result->SetStringField(TEXT("error"), Error);
	return Result;
}

//------------------------------------------------------------------------------
// Graph / node / pin lookups (Phase D)
//------------------------------------------------------------------------------

UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName, FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Null blueprint");
		return nullptr;
	}
	if (GraphName.IsEmpty())
	{
		OutError = TEXT("Empty graph name");
		return nullptr;
	}

	// Convenience aliases for the main event graph.
	const bool bWantsEventGraph =
		GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase) ||
		GraphName.Equals(TEXT("ubergraph"), ESearchCase::IgnoreCase);

	if (bWantsEventGraph && Blueprint->UbergraphPages.Num() > 0)
	{
		return Blueprint->UbergraphPages[0];
	}

	const FName NameLookup(*GraphName);

	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == NameLookup) { return Graph; }
	}
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetFName() == NameLookup) { return Graph; }
	}
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (Graph && Graph->GetFName() == NameLookup) { return Graph; }
	}
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph && Graph->GetFName() == NameLookup) { return Graph; }
	}

	OutError = FString::Printf(TEXT("Graph not found: %s"), *GraphName);
	return nullptr;
}

UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FGuid& NodeGuid)
{
	if (!Graph || !NodeGuid.IsValid()) { return nullptr; }
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == NodeGuid)
		{
			return Node;
		}
	}
	return nullptr;
}

UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
	if (!Node) { return nullptr; }
	const FName NameLookup(*PinName);
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName == NameLookup && Pin->Direction == Direction)
		{
			return Pin;
		}
	}
	// Fallback: allow lookup without direction when caller passes EGPD_MAX-equivalent.
	// Callers always pass a concrete direction, so we don't retry unprompted.
	return nullptr;
}

TSharedPtr<FJsonObject> NodeToEditResponse(UEdGraphNode* Node)
{
	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
	if (!Node)
	{
		return Obj;
	}

	Obj->SetStringField(TEXT("guid"), Node->NodeGuid.ToString());
	Obj->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
	Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	Obj->SetNumberField(TEXT("x"), Node->NodePosX);
	Obj->SetNumberField(TEXT("y"), Node->NodePosY);

	TArray<TSharedPtr<FJsonValue>> PinsArr;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) { continue; }
		TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject);
		PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			PinObj->SetStringField(TEXT("subcategory_object"), Pin->PinType.PinSubCategoryObject->GetName());
		}
		PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		PinObj->SetBoolField(TEXT("connected"), Pin->LinkedTo.Num() > 0);
		PinsArr.Add(MakeShareable(new FJsonValueObject(PinObj)));
	}
	Obj->SetArrayField(TEXT("pins"), PinsArr);

	return Obj;
}

} // namespace FBlueprintEditHelpers
