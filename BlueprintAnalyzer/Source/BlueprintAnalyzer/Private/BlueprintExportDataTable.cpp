// BlueprintExportDataTable.cpp
// Read-only inspection of UDataTable assets (schema, row keys, individual
// rows, full dumps). Loads the table directly via LoadObject<UDataTable>; no
// UBlueprint indirection is involved, so the loader strictness that affects
// `edit cdo get` doesn't apply here.
//
// Per-row values are stringified via FProperty::ExportTextItem (the same UE
// text format that `cdo get` returns), so nested structs/arrays/object refs
// round-trip in a form callers already know how to parse.

#include "BlueprintExportCommandlet.h"

#include "Engine/DataTable.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Unity-build note: free functions in anonymous namespaces collide across .cpp
// files merged into one TU. Use a unique DataTable_ prefix on locals.

namespace
{
	// Build a {"success": false, "error": msg} envelope.
	TSharedPtr<FJsonObject> DataTable_MakeError(const FString& Message)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		Out->SetBoolField(TEXT("success"), false);
		Out->SetStringField(TEXT("error"), Message);
		return Out;
	}

	// Load the table; on failure return nullptr and populate OutError with an
	// error envelope ready to forward to the client.
	UDataTable* DataTable_Load(const FString& Path, TSharedPtr<FJsonObject>& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = DataTable_MakeError(TEXT("Empty path"));
			return nullptr;
		}
		UDataTable* Table = LoadObject<UDataTable>(nullptr, *Path);
		if (!Table)
		{
			OutError = DataTable_MakeError(FString::Printf(TEXT("Failed to load DataTable at path: %s"), *Path));
			return nullptr;
		}
		return Table;
	}

	// Stringify one row as { col_name: exported_text, ... }.
	TSharedPtr<FJsonObject> DataTable_RowToJson(UScriptStruct* Struct, const uint8* RowData)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		if (!Struct || !RowData) { return Out; }
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop) { continue; }
			FString Exported;
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);
			// UE4.27: FProperty::ExportTextItem(ValueStr, PropertyValue, DefaultValue, Parent, PortFlags)
			Prop->ExportTextItem(Exported, ValuePtr, /*DefaultValue*/nullptr, /*Parent*/nullptr, PPF_None);
			Out->SetStringField(Prop->GetName(), Exported);
		}
		return Out;
	}

	// Standard {success, path, [row_struct, ...]} preamble shared across responses.
	TSharedPtr<FJsonObject> DataTable_MakeSuccess(UDataTable* Table)
	{
		TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("path"), Table->GetPathName());
		if (UScriptStruct* Struct = Table->RowStruct)
		{
			Result->SetStringField(TEXT("row_struct"), Struct->GetName());
		}
		return Result;
	}
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::DataTableSchemaToJson(const FString& Path)
{
	TSharedPtr<FJsonObject> Err;
	UDataTable* Table = DataTable_Load(Path, Err);
	if (!Table) { return Err; }

	TSharedPtr<FJsonObject> Result = DataTable_MakeSuccess(Table);
	UScriptStruct* Struct = Table->RowStruct;
	if (!Struct)
	{
		// Tables with a deleted RowStruct still load, but have no schema to report.
		Result->SetStringField(TEXT("row_struct"), TEXT(""));
		Result->SetArrayField(TEXT("columns"), TArray<TSharedPtr<FJsonValue>>());
		return Result;
	}

	Result->SetStringField(TEXT("row_struct_path"), Struct->GetPathName());

	TArray<TSharedPtr<FJsonValue>> Columns;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop) { continue; }
		TSharedPtr<FJsonObject> ColObj = MakeShareable(new FJsonObject);
		ColObj->SetStringField(TEXT("name"), Prop->GetName());
		ColObj->SetStringField(TEXT("type"), Prop->GetCPPType());
		ColObj->SetStringField(TEXT("class"), Prop->GetClass()->GetName());
		Columns.Add(MakeShareable(new FJsonValueObject(ColObj)));
	}
	Result->SetArrayField(TEXT("columns"), Columns);
	return Result;
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::DataTableRowsToJson(const FString& Path)
{
	TSharedPtr<FJsonObject> Err;
	UDataTable* Table = DataTable_Load(Path, Err);
	if (!Table) { return Err; }

	TSharedPtr<FJsonObject> Result = DataTable_MakeSuccess(Table);
	TArray<TSharedPtr<FJsonValue>> Keys;
	for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
	{
		Keys.Add(MakeShareable(new FJsonValueString(Pair.Key.ToString())));
	}
	Result->SetArrayField(TEXT("row_keys"), Keys);
	Result->SetNumberField(TEXT("row_count"), Keys.Num());
	return Result;
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::DataTableGetRowToJson(const FString& Path, const FString& RowKey)
{
	TSharedPtr<FJsonObject> Err;
	UDataTable* Table = DataTable_Load(Path, Err);
	if (!Table) { return Err; }

	if (RowKey.IsEmpty())
	{
		return DataTable_MakeError(TEXT("Empty row key"));
	}
	if (!Table->RowStruct)
	{
		return DataTable_MakeError(FString::Printf(TEXT("Table %s has no RowStruct"), *Path));
	}

	const FName RowName(*RowKey);
	uint8* const* RowPtr = Table->GetRowMap().Find(RowName);
	if (!RowPtr || !*RowPtr)
	{
		return DataTable_MakeError(FString::Printf(TEXT("Row not found: %s"), *RowKey));
	}

	TSharedPtr<FJsonObject> Result = DataTable_MakeSuccess(Table);
	Result->SetStringField(TEXT("row_key"), RowKey);
	Result->SetObjectField(TEXT("row_data"), DataTable_RowToJson(Table->RowStruct, *RowPtr));
	return Result;
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::DataTableDumpToJson(const FString& Path)
{
	TSharedPtr<FJsonObject> Err;
	UDataTable* Table = DataTable_Load(Path, Err);
	if (!Table) { return Err; }

	TSharedPtr<FJsonObject> Result = DataTable_MakeSuccess(Table);
	UScriptStruct* Struct = Table->RowStruct;
	TSharedPtr<FJsonObject> RowsObj = MakeShareable(new FJsonObject);
	int32 RowCount = 0;
	if (Struct)
	{
		for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
		{
			if (!Pair.Value) { continue; }
			RowsObj->SetObjectField(Pair.Key.ToString(), DataTable_RowToJson(Struct, Pair.Value));
			++RowCount;
		}
	}
	Result->SetObjectField(TEXT("rows"), RowsObj);
	Result->SetNumberField(TEXT("row_count"), RowCount);
	return Result;
}
