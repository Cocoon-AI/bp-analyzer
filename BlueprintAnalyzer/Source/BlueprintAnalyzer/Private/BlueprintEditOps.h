// BlueprintEditOps.h
// FBlueprintEditOps is a static-method family that implements all Blueprint
// mutation operations exposed by the analyzer plugin. The methods are split
// across several .cpp files by topic (metadata, variables, functions, ...)
// for maintainability; this header declares the full surface area in one place.
//
// All methods take a JSON params object (as delivered from the server) and
// return a JSON result object shaped as either:
//     { "success": true,  "path": <path>, ...op-specific fields }
//     { "success": false, "error": <message> }
//
// The server wraps the returned result in a standard JSON-RPC 2.0 envelope.
// Edit ops never return a JSON-RPC "error" response themselves — every failure
// (including missing required params) is reported as { "success": false, "error": ... }.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

class FBlueprintEditOps
{
public:
	// --- Phase A: lifecycle + metadata (BlueprintEditOps_Metadata.cpp) ---

	static TSharedPtr<FJsonObject> Compile(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> Save(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> SaveAndCompile(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> Reparent(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> AddInterface(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> RemoveInterface(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> SetFlags(const TSharedPtr<FJsonObject>& Params);

	// --- Phase B: variables + CDO (BlueprintEditOps_Variables.cpp) ---

	static TSharedPtr<FJsonObject> VariableList(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> PurgePhantom(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> VariableAdd(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> VariableRemove(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> VariableRename(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> VariableUnshadow(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> VariableLift(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> VariableSetType(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> VariableSetDefault(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> VariableSetFlags(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> VariableSetMetadata(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> CdoSetProperty(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> CdoGetProperty(const TSharedPtr<FJsonObject>& Params);

	// --- Phase C: functions, events, components ---

	// BlueprintEditOps_Functions.cpp
	static TSharedPtr<FJsonObject> FunctionAdd(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> FunctionRemove(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> FunctionRename(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> FunctionAddParam(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> FunctionRemoveParam(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> FunctionSetFlags(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> FunctionOverride(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> EventAddCustom(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> EventRemove(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> EventImplement(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> DispatcherRemove(const TSharedPtr<FJsonObject>& Params);

	// BlueprintEditOps_Components.cpp
	static TSharedPtr<FJsonObject> ComponentAdd(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> ComponentRemove(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> ComponentReparent(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> ComponentSetProperty(const TSharedPtr<FJsonObject>& Params);

	// --- Phase D: node graph editing ---

	// BlueprintEditOps_Nodes.cpp (generic node/pin ops)
	static TSharedPtr<FJsonObject> NodeRemove(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> NodeRemoveBroken(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> NodeMove(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> NodeAddGeneric(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> PinSetDefault(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> PinConnect(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> PinDisconnect(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> PinBreakAllLinks(const TSharedPtr<FJsonObject>& Params);

	// BlueprintEditOps_NodeBuilders.cpp (high-level curated K2 node builders)
	static TSharedPtr<FJsonObject> NodeAddVariableGet(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> NodeAddVariableSet(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> NodeAddFunctionCall(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> NodeAddBranch(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> NodeAddSequence(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> NodeAddForLoop(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> NodeAddCast(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> NodeAddMakeStruct(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> NodeAddBreakStruct(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> NodeAddLiteral(const TSharedPtr<FJsonObject>& Params);
};

/** Free function called by FBlueprintExportServer::DispatchRequest for any method
 *  whose name begins with "edit.". Lives in BlueprintExportServerEditDispatch.cpp. */
TSharedPtr<FJsonObject> DispatchEditRequest(const FString& Method, const TSharedPtr<FJsonObject>& Params);
