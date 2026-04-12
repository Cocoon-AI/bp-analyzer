// BlueprintExportServer.cpp
// Named pipe server implementation for persistent Blueprint analysis

#include "BlueprintExportServer.h"
#include "BlueprintExportCommandlet.h"
#include "BlueprintEditOps.h"
#include "UObject/UObjectGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>

#include "Policies/CondensedJsonPrintPolicy.h"

// JSON-RPC 2.0 error codes
#define JSONRPC_PARSE_ERROR      -32700
#define JSONRPC_INVALID_REQUEST  -32600
#define JSONRPC_METHOD_NOT_FOUND -32601
#define JSONRPC_INVALID_PARAMS   -32602
#define JSONRPC_SERVER_ERROR     -32000

FBlueprintExportServer::FBlueprintExportServer(UBlueprintExportCommandlet* InCommandlet, const FString& InPipeName)
	: Commandlet(InCommandlet)
	, PipeName(InPipeName)
	, PipeHandle(INVALID_HANDLE_VALUE)
	, OverlapEvent(NULL)
	, bRunning(false)
{
}

FBlueprintExportServer::~FBlueprintExportServer()
{
	Stop();
	if (OverlapEvent != NULL)
	{
		CloseHandle(OverlapEvent);
		OverlapEvent = NULL;
	}
	if (PipeHandle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(PipeHandle);
		PipeHandle = INVALID_HANDLE_VALUE;
	}
}

bool FBlueprintExportServer::Start()
{
	FString FullPipeName = FString::Printf(TEXT("\\\\.\\pipe\\%s"), *PipeName);

	OverlapEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (OverlapEvent == NULL)
	{
		UE_LOG(LogTemp, Error, TEXT("BlueprintExportServer: Failed to create event object"));
		return false;
	}

	PipeHandle = CreateNamedPipeW(
		*FullPipeName,
		PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		1,       // max instances
		65536,   // out buffer
		65536,   // in buffer
		0,       // default timeout
		NULL     // default security
	);

	if (PipeHandle == INVALID_HANDLE_VALUE)
	{
		UE_LOG(LogTemp, Error, TEXT("BlueprintExportServer: Failed to create named pipe '%s' (error %d)"), *FullPipeName, GetLastError());
		return false;
	}

	bRunning = true;
	UE_LOG(LogTemp, Display, TEXT("BlueprintExportServer: Listening on %s"), *FullPipeName);
	return true;
}

void FBlueprintExportServer::Run()
{
	while (bRunning)
	{
		// Wait for a client to connect using overlapped I/O so we can check bRunning
		OVERLAPPED Overlapped = {};
		Overlapped.hEvent = OverlapEvent;
		ResetEvent(OverlapEvent);

		BOOL Connected = ConnectNamedPipe(PipeHandle, &Overlapped);
		if (!Connected)
		{
			DWORD Err = GetLastError();
			if (Err == ERROR_IO_PENDING)
			{
				// Wait with periodic timeout to check bRunning
				while (bRunning)
				{
					DWORD WaitResult = WaitForSingleObject(OverlapEvent, 1000);
					if (WaitResult == WAIT_OBJECT_0)
					{
						break; // Client connected
					}
					// WAIT_TIMEOUT: loop and check bRunning
				}
				if (!bRunning)
				{
					CancelIo(PipeHandle);
					break;
				}
			}
			else if (Err == ERROR_PIPE_CONNECTED)
			{
				// Client already connected before ConnectNamedPipe was called
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("BlueprintExportServer: ConnectNamedPipe failed (error %d)"), Err);
				FPlatformProcess::Sleep(1.0f);
				continue;
			}
		}

		UE_LOG(LogTemp, Display, TEXT("BlueprintExportServer: Client connected"));

		// Inner loop: handle requests from connected client
		while (bRunning)
		{
			FString RequestStr;
			if (!ReadMessage(RequestStr))
			{
				// Client disconnected or read error
				break;
			}

			// Parse JSON
			TSharedPtr<FJsonObject> RequestJson;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestStr);
			if (!FJsonSerializer::Deserialize(Reader, RequestJson) || !RequestJson.IsValid())
			{
				TSharedPtr<FJsonObject> ErrorResp = MakeErrorResponse(
					MakeShareable(new FJsonValueNull()),
					JSONRPC_PARSE_ERROR,
					TEXT("Parse error: invalid JSON")
				);
				WriteMessage(JsonToString(ErrorResp));
				continue;
			}

			// Dispatch and respond
			TSharedPtr<FJsonObject> Response = DispatchRequest(RequestJson);
			if (!Response.IsValid())
			{
				Response = MakeErrorResponse(
					RequestJson->TryGetField(TEXT("id")),
					JSONRPC_SERVER_ERROR,
					TEXT("Internal error: null response from dispatch")
				);
			}
			FString ResponseStr = JsonToString(Response);
			if (!WriteMessage(ResponseStr))
			{
				UE_LOG(LogTemp, Warning, TEXT("BlueprintExportServer: Failed to write response (%d bytes)"), ResponseStr.Len());
				break;
			}

			// Run garbage collection periodically to prevent memory buildup in server mode
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}

		// Disconnect and wait for next client
		DisconnectNamedPipe(PipeHandle);
		UE_LOG(LogTemp, Display, TEXT("BlueprintExportServer: Client disconnected"));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintExportServer: Server stopped"));
}

void FBlueprintExportServer::Stop()
{
	bRunning = false;
}

bool FBlueprintExportServer::ReadMessage(FString& OutMessage)
{
	// Read 4-byte length prefix
	uint32 MessageLength = 0;
	DWORD BytesRead = 0;
	if (!ReadFile(PipeHandle, &MessageLength, sizeof(uint32), &BytesRead, NULL) || BytesRead != sizeof(uint32))
	{
		return false;
	}

	// Sanity check (max 64MB)
	if (MessageLength > 64 * 1024 * 1024)
	{
		UE_LOG(LogTemp, Error, TEXT("BlueprintExportServer: Message too large (%u bytes)"), MessageLength);
		return false;
	}

	// Read the payload
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(MessageLength);
	DWORD TotalRead = 0;
	while (TotalRead < MessageLength)
	{
		DWORD ChunkRead = 0;
		if (!ReadFile(PipeHandle, Buffer.GetData() + TotalRead, MessageLength - TotalRead, &ChunkRead, NULL) || ChunkRead == 0)
		{
			return false;
		}
		TotalRead += ChunkRead;
	}

	// Convert UTF-8 to FString
	FUTF8ToTCHAR Converter((const ANSICHAR*)Buffer.GetData(), MessageLength);
	OutMessage = FString(Converter.Length(), Converter.Get());
	return true;
}

bool FBlueprintExportServer::WriteMessage(const FString& Message)
{
	// Convert FString to UTF-8
	FTCHARToUTF8 Converter(*Message);
	uint32 PayloadLength = (uint32)Converter.Length();

	// Write 4-byte length prefix
	DWORD BytesWritten = 0;
	if (!WriteFile(PipeHandle, &PayloadLength, sizeof(uint32), &BytesWritten, NULL) || BytesWritten != sizeof(uint32))
	{
		return false;
	}

	// Write the payload
	DWORD TotalWritten = 0;
	while (TotalWritten < PayloadLength)
	{
		DWORD ChunkWritten = 0;
		if (!WriteFile(PipeHandle, Converter.Get() + TotalWritten, PayloadLength - TotalWritten, &ChunkWritten, NULL) || ChunkWritten == 0)
		{
			return false;
		}
		TotalWritten += ChunkWritten;
	}

	return true;
}

TSharedPtr<FJsonObject> FBlueprintExportServer::DispatchRequest(const TSharedPtr<FJsonObject>& Request)
{
	// Extract JSON-RPC fields
	TSharedPtr<FJsonValue> Id = Request->TryGetField(TEXT("id"));
	if (!Id.IsValid())
	{
		Id = MakeShareable(new FJsonValueNull());
	}

	FString JsonRpcVersion;
	if (!Request->TryGetStringField(TEXT("jsonrpc"), JsonRpcVersion) || JsonRpcVersion != TEXT("2.0"))
	{
		return MakeErrorResponse(Id, JSONRPC_INVALID_REQUEST, TEXT("Invalid request: missing or wrong jsonrpc version"));
	}

	FString Method;
	if (!Request->TryGetStringField(TEXT("method"), Method))
	{
		return MakeErrorResponse(Id, JSONRPC_INVALID_REQUEST, TEXT("Invalid request: missing method"));
	}

	// Get params (optional, default to empty object)
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	TSharedPtr<FJsonObject> Params;
	if (Request->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
	{
		Params = *ParamsPtr;
	}
	else
	{
		Params = MakeShareable(new FJsonObject);
	}

	// --- Dispatch by method name ---

	// Forward any "edit.*" method to the edit-ops dispatch table. Keeps the main
	// if-ladder scoped to read operations.
	if (Method.StartsWith(TEXT("edit.")))
	{
		TSharedPtr<FJsonObject> EditResult = DispatchEditRequest(Method, Params);
		if (!EditResult.IsValid())
		{
			EditResult = MakeShareable(new FJsonObject);
			EditResult->SetBoolField(TEXT("success"), false);
			EditResult->SetStringField(TEXT("error"), TEXT("DispatchEditRequest returned null"));
		}
		return MakeResponse(Id, EditResult);
	}

	if (Method == TEXT("ping"))
	{
		TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
		Result->SetBoolField(TEXT("pong"), true);
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("shutdown"))
	{
		bRunning = false;
		TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("message"), TEXT("Server shutting down"));
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("build_info"))
	{
		// __DATE__ and __TIME__ are baked in at TU compile time, so this reports
		// when the BlueprintExportServer.cpp translation unit was last compiled.
		// Close enough to "when was the plugin rebuilt" for diagnosing client-
		// side staleness. See versionCmd in cmd/digbp/main.go for the consumer.
		TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("build_date"), UTF8_TO_TCHAR(__DATE__));
		Result->SetStringField(TEXT("build_time"), UTF8_TO_TCHAR(__TIME__));
		Result->SetStringField(TEXT("module_name"), TEXT("BlueprintAnalyzer"));
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("export"))
	{
		FString Path;
		if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: path"));
		}

		bool bAnalyze = false;
		Params->TryGetBoolField(TEXT("analyze"), bAnalyze);

		FString Mode;
		Params->TryGetStringField(TEXT("mode"), Mode);

		// If mode is compact or skeleton, return text in a JSON wrapper
		if (Mode == TEXT("compact") || Mode == TEXT("skeleton"))
		{
			EBlueprintExportMode ExportMode = (Mode == TEXT("skeleton"))
				? EBlueprintExportMode::Skeleton
				: EBlueprintExportMode::Compact;

			FString Text = Commandlet->ExportBlueprintToText(Path, ExportMode, bAnalyze);

			TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
			Result->SetBoolField(TEXT("success"), !Text.StartsWith(TEXT("Error:")));
			Result->SetStringField(TEXT("mode"), Mode);
			Result->SetStringField(TEXT("output"), Text);
			return MakeResponse(Id, Result);
		}

		// Default: JSON mode
		TSharedPtr<FJsonObject> Result = Commandlet->ExportBlueprintToJson(Path, bAnalyze);
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("list"))
	{
		FString Dir;
		if (!Params->TryGetStringField(TEXT("dir"), Dir) || Dir.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: dir"));
		}

		bool bRecursive = true;
		Params->TryGetBoolField(TEXT("recursive"), bRecursive);

		TSharedPtr<FJsonObject> Result = Commandlet->ExportDirectoryToJson(Dir, bRecursive);
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("cppusage"))
	{
		FString Path;
		if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: path"));
		}

		TSharedPtr<FJsonObject> Result = Commandlet->GetCppUsageToJson(Path);
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("references"))
	{
		FString Path;
		if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: path"));
		}

		TSharedPtr<FJsonObject> Result = Commandlet->GetReferencesToJson(Path);
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("graph"))
	{
		FString Path;
		if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: path"));
		}

		int32 Depth = 3;
		double DepthVal;
		if (Params->TryGetNumberField(TEXT("depth"), DepthVal))
		{
			Depth = (int32)DepthVal;
		}

		TSharedPtr<FJsonObject> Result = Commandlet->ExportGraphToJson(Path, Depth);
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("refview"))
	{
		FString Path;
		if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: path"));
		}

		int32 RefDepth = 3;
		int32 ReferDepth = 3;
		bool bBpOnly = false;
		double TmpVal;
		if (Params->TryGetNumberField(TEXT("refdepth"), TmpVal)) { RefDepth = (int32)TmpVal; }
		if (Params->TryGetNumberField(TEXT("referdepth"), TmpVal)) { ReferDepth = (int32)TmpVal; }
		Params->TryGetBoolField(TEXT("bponly"), bBpOnly);

		TSharedPtr<FJsonObject> Result = Commandlet->ExportRefViewToJson(Path, RefDepth, ReferDepth, bBpOnly);
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("findcallers"))
	{
		FString Dir;
		if (!Params->TryGetStringField(TEXT("dir"), Dir) || Dir.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: dir"));
		}

		FString Func;
		if (!Params->TryGetStringField(TEXT("func"), Func) || Func.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: func"));
		}

		FString ClassName;
		Params->TryGetStringField(TEXT("class"), ClassName);

		TArray<FString> SearchPaths;
		SearchPaths.Add(Dir);

		TSharedPtr<FJsonObject> Result = Commandlet->FindCallersToJson(Func, ClassName, SearchPaths);
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("findvaruses"))
	{
		FString Dir;
		if (!Params->TryGetStringField(TEXT("dir"), Dir) || Dir.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: dir"));
		}

		FString Var;
		if (!Params->TryGetStringField(TEXT("var"), Var) || Var.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: var"));
		}

		FString Kind;
		Params->TryGetStringField(TEXT("kind"), Kind);

		TArray<FString> SearchPaths;
		SearchPaths.Add(Dir);

		TSharedPtr<FJsonObject> Result = Commandlet->FindVarUsesToJson(Var, Kind, SearchPaths);
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("nativeevents"))
	{
		FString Dir;
		if (!Params->TryGetStringField(TEXT("dir"), Dir) || Dir.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: dir"));
		}

		TArray<FString> SearchPaths;
		SearchPaths.Add(Dir);

		TSharedPtr<FJsonObject> Result = Commandlet->FindNativeEventsToJson(SearchPaths);
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("findevents"))
	{
		FString Dir;
		if (!Params->TryGetStringField(TEXT("dir"), Dir) || Dir.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: dir"));
		}

		FString EventName;
		if (!Params->TryGetStringField(TEXT("event"), EventName) || EventName.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: event"));
		}

		TArray<FString> SearchPaths;
		SearchPaths.Add(Dir);

		TSharedPtr<FJsonObject> Result = Commandlet->FindImplementableEventsToJson(EventName, SearchPaths);
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("findprop"))
	{
		FString Dir;
		if (!Params->TryGetStringField(TEXT("dir"), Dir) || Dir.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: dir"));
		}

		FString PropName;
		if (!Params->TryGetStringField(TEXT("prop"), PropName) || PropName.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: prop"));
		}

		FString PropValue;
		Params->TryGetStringField(TEXT("value"), PropValue);

		FString ParentClass;
		Params->TryGetStringField(TEXT("parentclass"), ParentClass);

		TArray<FString> SearchPaths;
		SearchPaths.Add(Dir);

		TSharedPtr<FJsonObject> Result = Commandlet->FindPropertyToJson(PropName, PropValue, ParentClass, SearchPaths);
		return MakeResponse(Id, Result);
	}

	if (Method == TEXT("search"))
	{
		FString Dir;
		if (!Params->TryGetStringField(TEXT("dir"), Dir) || Dir.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: dir"));
		}

		FString Query;
		if (!Params->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: query"));
		}

		TArray<FString> SearchPaths;
		SearchPaths.Add(Dir);

		TSharedPtr<FJsonObject> Result = Commandlet->SearchInBlueprintsToJson(Query, SearchPaths);
		return MakeResponse(Id, Result);
	}

	return MakeErrorResponse(Id, JSONRPC_METHOD_NOT_FOUND, FString::Printf(TEXT("Method not found: %s"), *Method));
}

TSharedPtr<FJsonObject> FBlueprintExportServer::MakeResponse(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Result)
{
	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetField(TEXT("id"), Id);
	Response->SetObjectField(TEXT("result"), Result);
	return Response;
}

TSharedPtr<FJsonObject> FBlueprintExportServer::MakeErrorResponse(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> ErrorObj = MakeShareable(new FJsonObject);
	ErrorObj->SetNumberField(TEXT("code"), Code);
	ErrorObj->SetStringField(TEXT("message"), Message);

	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetField(TEXT("id"), Id);
	Response->SetObjectField(TEXT("error"), ErrorObj);
	return Response;
}

FString FBlueprintExportServer::JsonToString(const TSharedPtr<FJsonObject>& JsonObject)
{
	FString OutputString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	return OutputString;
}

#include "Windows/HideWindowsPlatformTypes.h"
