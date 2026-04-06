// BlueprintExportServer.h
// Named pipe server for persistent Blueprint analysis via JSON-RPC 2.0

#pragma once

#include "CoreMinimal.h"

class FJsonValue;
class FJsonObject;
// Forward declare Windows HANDLE type to avoid polluting headers
typedef void* HANDLE;

class UBlueprintExportCommandlet;

/**
 * Named pipe server that accepts JSON-RPC 2.0 requests and dispatches
 * them to the BlueprintExportCommandlet's ToJson methods.
 *
 * Transport: Windows Named Pipe (\\.\pipe\<PipeName>)
 * Framing: 4-byte little-endian uint32 length prefix + UTF-8 JSON payload
 * Protocol: JSON-RPC 2.0
 *
 * Single-client, blocking. When one client disconnects, waits for the next.
 */
class FBlueprintExportServer
{
public:
	FBlueprintExportServer(UBlueprintExportCommandlet* InCommandlet, const FString& InPipeName);
	~FBlueprintExportServer();

	/** Create the named pipe. Returns true on success. */
	bool Start();

	/** Blocking server loop. Returns when shutdown is requested. */
	void Run();

	/** Signal the server to stop. */
	void Stop();

private:
	/** Parse a JSON-RPC request and dispatch to the appropriate operation. */
	TSharedPtr<FJsonObject> DispatchRequest(const TSharedPtr<FJsonObject>& Request);

	/** Read a length-prefixed message from the connected client. */
	bool ReadMessage(FString& OutMessage);

	/** Write a length-prefixed message to the connected client. */
	bool WriteMessage(const FString& Message);

	/** Build a JSON-RPC 2.0 success response. */
	TSharedPtr<FJsonObject> MakeResponse(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Result);

	/** Build a JSON-RPC 2.0 error response. */
	TSharedPtr<FJsonObject> MakeErrorResponse(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message);

	/** Serialize a JSON object to string. */
	FString JsonToString(const TSharedPtr<FJsonObject>& JsonObject);

	UBlueprintExportCommandlet* Commandlet;
	FString PipeName;
	HANDLE PipeHandle;
	HANDLE OverlapEvent;
	bool bRunning;
};
