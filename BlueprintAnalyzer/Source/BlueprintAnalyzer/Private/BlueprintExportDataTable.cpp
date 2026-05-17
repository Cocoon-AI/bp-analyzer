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
#include "UObject/SoftObjectPtr.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FileHelpers.h"

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

//------------------------------------------------------------------------------
// Write-side helpers
//------------------------------------------------------------------------------

namespace
{
	// Mutable row-pointer accessor. GetRowMap() returns const; we cast through
	// it to write. UE4.27 has no public WriteRowMap accessor, and the editor's
	// DataTableEditorUtils takes the same approach (FDataTableEditorUtils::
	// AddRow / RemoveRow operate on Table->RowMap directly).
	uint8* DataTable_FindRowMutable(UDataTable* Table, FName RowName)
	{
		if (!Table) { return nullptr; }
		uint8* const* RowPtr = Table->GetRowMap().Find(RowName);
		return (RowPtr && *RowPtr) ? *RowPtr : nullptr;
	}

	// Single-cell write. Returns "" on success, error message on failure.
	// Empty-paren TArray fix: ImportText for FArrayProperty with value "()" can
	// produce a single-element array containing a default-constructed inner
	// (observed: TArray<FName>="()" reads back as ("")). Special-case "()" to
	// explicitly empty the array via FScriptArrayHelper.
	FString DataTable_WriteCell(UScriptStruct* RowStruct, uint8* RowData, const FString& Column, const FString& Value)
	{
		if (!RowStruct || !RowData)
		{
			return TEXT("Null row struct or data");
		}
		FProperty* Prop = RowStruct->FindPropertyByName(FName(*Column));
		if (!Prop)
		{
			return FString::Printf(TEXT("Column not found on %s: %s"), *RowStruct->GetName(), *Column);
		}
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);

		// Empty-array workaround. ImportText("()" -> FArrayProperty) is buggy
		// in this UE4.27 revision; bypass the parser and just empty the array.
		if (Value.TrimStartAndEnd() == TEXT("()"))
		{
			if (FArrayProperty* AP = CastField<FArrayProperty>(Prop))
			{
				FScriptArrayHelper Helper(AP, ValuePtr);
				Helper.EmptyValues();
				return FString();
			}
			if (FSetProperty* SP = CastField<FSetProperty>(Prop))
			{
				FScriptSetHelper Helper(SP, ValuePtr);
				Helper.EmptyElements();
				return FString();
			}
			if (FMapProperty* MP = CastField<FMapProperty>(Prop))
			{
				FScriptMapHelper Helper(MP, ValuePtr);
				Helper.EmptyValues();
				return FString();
			}
			// Not a container — fall through to ImportText so e.g. an FString
			// literal "()" still round-trips.
		}

		const TCHAR* ImportResult = Prop->ImportText(*Value, ValuePtr, PPF_None, /*Parent*/nullptr);
		if (ImportResult == nullptr)
		{
			return FString::Printf(TEXT("ImportText failed for %s = '%s'"), *Column, *Value);
		}
		return FString();
	}
}

//------------------------------------------------------------------------------
// datatable.set — single cell
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::DataTableSetCellToJson(const FString& Path, const FString& RowKey, const FString& Column, const FString& Value)
{
	TSharedPtr<FJsonObject> Err;
	UDataTable* Table = DataTable_Load(Path, Err);
	if (!Table) { return Err; }

	if (RowKey.IsEmpty())   { return DataTable_MakeError(TEXT("Empty row key")); }
	if (Column.IsEmpty())   { return DataTable_MakeError(TEXT("Empty column")); }
	if (!Table->RowStruct)  { return DataTable_MakeError(FString::Printf(TEXT("Table %s has no RowStruct"), *Path)); }

	const FName RowName(*RowKey);
	uint8* RowData = DataTable_FindRowMutable(Table, RowName);
	if (!RowData)
	{
		return DataTable_MakeError(FString::Printf(TEXT("Row not found: %s"), *RowKey));
	}

	FString WriteError = DataTable_WriteCell(Table->RowStruct, RowData, Column, Value);
	if (!WriteError.IsEmpty())
	{
		return DataTable_MakeError(WriteError);
	}

	Table->HandleDataTableChanged(RowName);
	Table->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = DataTable_MakeSuccess(Table);
	Result->SetStringField(TEXT("row_key"), RowKey);
	Result->SetStringField(TEXT("column"), Column);
	Result->SetStringField(TEXT("value"), Value);
	Result->SetBoolField(TEXT("dirty"), true);
	return Result;
}

//------------------------------------------------------------------------------
// datatable.set_many — batch, all-or-nothing
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::DataTableSetManyToJson(const FString& Path, const TSharedPtr<FJsonObject>& Updates)
{
	TSharedPtr<FJsonObject> Err;
	UDataTable* Table = DataTable_Load(Path, Err);
	if (!Table) { return Err; }

	if (!Updates.IsValid())  { return DataTable_MakeError(TEXT("Missing or invalid 'rows' object")); }
	if (!Table->RowStruct)   { return DataTable_MakeError(FString::Printf(TEXT("Table %s has no RowStruct"), *Path)); }

	// Pre-pass: validate every (row, col) target exists. This is the all-or-
	// nothing guarantee — surface the first lookup failure before we mutate
	// anything. ImportText failures can still happen mid-batch (one bad value
	// won't fail-fast through pre-validation), in which case the partial
	// writes that already landed stay landed; we return success=false with
	// the failing row/col so the caller can diagnose.
	for (const auto& RowPair : Updates->Values)
	{
		const FString& RowKey = RowPair.Key;
		const TSharedPtr<FJsonObject>* ColsObj = nullptr;
		if (!RowPair.Value.IsValid() || !RowPair.Value->TryGetObject(ColsObj) || !ColsObj || !ColsObj->IsValid())
		{
			return DataTable_MakeError(FString::Printf(TEXT("Updates['%s'] is not an object"), *RowKey));
		}
		const FName RowName(*RowKey);
		if (!Table->GetRowMap().Contains(RowName))
		{
			return DataTable_MakeError(FString::Printf(TEXT("Row not found: %s"), *RowKey));
		}
		for (const auto& ColPair : (*ColsObj)->Values)
		{
			if (!Table->RowStruct->FindPropertyByName(FName(*ColPair.Key)))
			{
				return DataTable_MakeError(FString::Printf(TEXT("Column not found on row '%s': %s"), *RowKey, *ColPair.Key));
			}
		}
	}

	// Apply pass. Track first failure (if any).
	int32 CellsApplied = 0;
	int32 RowsTouched = 0;
	TArray<FName> ChangedRows;
	for (const auto& RowPair : Updates->Values)
	{
		const FString& RowKey = RowPair.Key;
		const FName RowName(*RowKey);
		uint8* RowData = DataTable_FindRowMutable(Table, RowName);
		const TSharedPtr<FJsonObject>* ColsObj = nullptr;
		RowPair.Value->TryGetObject(ColsObj);

		bool bRowTouched = false;
		for (const auto& ColPair : (*ColsObj)->Values)
		{
			FString StrValue;
			if (!ColPair.Value->TryGetString(StrValue))
			{
				// Coerce non-string JSON values (numbers, bools) to string for
				// ImportText. UE's text-format is string-native anyway.
				StrValue = ColPair.Value->AsString();
			}
			FString WriteError = DataTable_WriteCell(Table->RowStruct, RowData, ColPair.Key, StrValue);
			if (!WriteError.IsEmpty())
			{
				if (bRowTouched) { ChangedRows.AddUnique(RowName); }
				if (ChangedRows.Num() > 0)
				{
					for (const FName& Touched : ChangedRows) { Table->HandleDataTableChanged(Touched); }
					Table->MarkPackageDirty();
				}
				TSharedPtr<FJsonObject> R = DataTable_MakeError(WriteError);
				R->SetStringField(TEXT("failed_row"), RowKey);
				R->SetStringField(TEXT("failed_column"), ColPair.Key);
				R->SetNumberField(TEXT("cells_applied_before_failure"), CellsApplied);
				return R;
			}
			++CellsApplied;
			bRowTouched = true;
		}
		if (bRowTouched)
		{
			ChangedRows.AddUnique(RowName);
			++RowsTouched;
		}
	}

	for (const FName& Touched : ChangedRows) { Table->HandleDataTableChanged(Touched); }
	if (CellsApplied > 0) { Table->MarkPackageDirty(); }

	TSharedPtr<FJsonObject> Result = DataTable_MakeSuccess(Table);
	Result->SetNumberField(TEXT("cells_applied"), CellsApplied);
	Result->SetNumberField(TEXT("rows_touched"), RowsTouched);
	Result->SetBoolField(TEXT("dirty"), CellsApplied > 0);
	return Result;
}

//------------------------------------------------------------------------------
// datatable.save — persist package
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::DataTableSaveToJson(const FString& Path)
{
	TSharedPtr<FJsonObject> Err;
	UDataTable* Table = DataTable_Load(Path, Err);
	if (!Table) { return Err; }

	UPackage* Package = Table->GetOutermost();
	if (!Package)
	{
		return DataTable_MakeError(TEXT("Table has no outer package"));
	}

	TSharedPtr<FJsonObject> Result = DataTable_MakeSuccess(Table);
	const bool bWasDirty = Package->IsDirty();
	if (!bWasDirty)
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

//------------------------------------------------------------------------------
// datatable.rewrite_paths — bulk soft-ref substring rewrite
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::DataTableRewritePathsToJson(const FString& Path, const FString& From, const FString& To)
{
	TSharedPtr<FJsonObject> Err;
	UDataTable* Table = DataTable_Load(Path, Err);
	if (!Table) { return Err; }

	if (From.IsEmpty())     { return DataTable_MakeError(TEXT("Empty 'from' substring")); }
	if (!Table->RowStruct)  { return DataTable_MakeError(FString::Printf(TEXT("Table %s has no RowStruct"), *Path)); }

	int32 RewrittenCount = 0;
	TArray<FName> ChangedRows;
	TArray<TSharedPtr<FJsonValue>> Changes;

	for (const TPair<FName, uint8*>& RowPair : Table->GetRowMap())
	{
		uint8* RowData = RowPair.Value;
		if (!RowData) { continue; }

		bool bRowTouched = false;
		for (TFieldIterator<FProperty> It(Table->RowStruct); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop) { continue; }

			// Cover both FSoftObjectProperty and FSoftClassProperty. The latter
			// inherits from the former.
			FSoftObjectProperty* SOP = CastField<FSoftObjectProperty>(Prop);
			if (!SOP) { continue; }

			void* ValuePtr = SOP->ContainerPtrToValuePtr<void>(RowData);
			FSoftObjectPtr* SoftPtr = static_cast<FSoftObjectPtr*>(ValuePtr);
			const FString CurrentPath = SoftPtr->ToString();
			if (CurrentPath.IsEmpty() || !CurrentPath.Contains(From)) { continue; }

			const FString NewPath = CurrentPath.Replace(*From, *To);
			SoftPtr->ResetWeakPtr();
			*SoftPtr = FSoftObjectPath(NewPath);
			++RewrittenCount;
			bRowTouched = true;

			TSharedPtr<FJsonObject> ChangeObj = MakeShareable(new FJsonObject);
			ChangeObj->SetStringField(TEXT("row"), RowPair.Key.ToString());
			ChangeObj->SetStringField(TEXT("column"), Prop->GetName());
			ChangeObj->SetStringField(TEXT("from"), CurrentPath);
			ChangeObj->SetStringField(TEXT("to"), NewPath);
			Changes.Add(MakeShareable(new FJsonValueObject(ChangeObj)));
		}
		if (bRowTouched) { ChangedRows.AddUnique(RowPair.Key); }
	}

	for (const FName& Touched : ChangedRows) { Table->HandleDataTableChanged(Touched); }
	if (RewrittenCount > 0) { Table->MarkPackageDirty(); }

	TSharedPtr<FJsonObject> Result = DataTable_MakeSuccess(Table);
	Result->SetNumberField(TEXT("rewritten_count"), RewrittenCount);
	Result->SetNumberField(TEXT("rows_touched"), ChangedRows.Num());
	Result->SetArrayField(TEXT("changes"), Changes);
	Result->SetBoolField(TEXT("dirty"), RewrittenCount > 0);
	return Result;
}
