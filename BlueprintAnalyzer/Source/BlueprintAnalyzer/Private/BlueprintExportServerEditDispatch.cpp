// BlueprintExportServerEditDispatch.cpp
// Dispatch table for all "edit.*" JSON-RPC methods. Called from
// FBlueprintExportServer::DispatchRequest via a single StartsWith("edit.")
// branch. Each method name maps to exactly one FBlueprintEditOps static method.
//
// Phase A methods are wired up now. Phases B/C/D append their routes to this
// same if-ladder; if it exceeds ~750 lines, split along the topic boundary
// suggested in the plan (metadata/variables/functions/components in this file,
// node editing in BlueprintExportServerEditDispatchNodes.cpp).

#include "BlueprintEditOps.h"
#include "BlueprintEditHelpers.h"

#include "Dom/JsonObject.h"

TSharedPtr<FJsonObject> DispatchEditRequest(const FString& Method, const TSharedPtr<FJsonObject>& Params)
{
	// --- Phase A: lifecycle + metadata ---

	if (Method == TEXT("edit.compile"))           { return FBlueprintEditOps::Compile(Params); }
	if (Method == TEXT("edit.save"))              { return FBlueprintEditOps::Save(Params); }
	if (Method == TEXT("edit.save_and_compile"))  { return FBlueprintEditOps::SaveAndCompile(Params); }
	if (Method == TEXT("edit.reparent"))          { return FBlueprintEditOps::Reparent(Params); }
	if (Method == TEXT("edit.add_interface"))     { return FBlueprintEditOps::AddInterface(Params); }
	if (Method == TEXT("edit.remove_interface"))  { return FBlueprintEditOps::RemoveInterface(Params); }
	if (Method == TEXT("edit.set_flags"))         { return FBlueprintEditOps::SetFlags(Params); }

	// --- Phase B: variables + CDO ---

	if (Method == TEXT("edit.variable.list"))         { return FBlueprintEditOps::VariableList(Params); }
	if (Method == TEXT("edit.variable.add"))          { return FBlueprintEditOps::VariableAdd(Params); }
	if (Method == TEXT("edit.variable.remove"))       { return FBlueprintEditOps::VariableRemove(Params); }
	if (Method == TEXT("edit.variable.rename"))       { return FBlueprintEditOps::VariableRename(Params); }
	if (Method == TEXT("edit.variable.unshadow"))     { return FBlueprintEditOps::VariableUnshadow(Params); }
	if (Method == TEXT("edit.variable.lift"))         { return FBlueprintEditOps::VariableLift(Params); }
	if (Method == TEXT("edit.variable.set_type"))     { return FBlueprintEditOps::VariableSetType(Params); }
	if (Method == TEXT("edit.variable.set_default"))  { return FBlueprintEditOps::VariableSetDefault(Params); }
	if (Method == TEXT("edit.variable.set_flags"))    { return FBlueprintEditOps::VariableSetFlags(Params); }
	if (Method == TEXT("edit.variable.set_metadata")) { return FBlueprintEditOps::VariableSetMetadata(Params); }
	if (Method == TEXT("edit.cdo.set_property"))      { return FBlueprintEditOps::CdoSetProperty(Params); }
	if (Method == TEXT("edit.cdo.get_property"))      { return FBlueprintEditOps::CdoGetProperty(Params); }
	if (Method == TEXT("edit.purge_phantom"))         { return FBlueprintEditOps::PurgePhantom(Params); }

	// --- Phase C: functions / events / components ---

	if (Method == TEXT("edit.function.add"))          { return FBlueprintEditOps::FunctionAdd(Params); }
	if (Method == TEXT("edit.function.remove"))       { return FBlueprintEditOps::FunctionRemove(Params); }
	if (Method == TEXT("edit.function.rename"))       { return FBlueprintEditOps::FunctionRename(Params); }
	if (Method == TEXT("edit.function.add_param"))    { return FBlueprintEditOps::FunctionAddParam(Params); }
	if (Method == TEXT("edit.function.remove_param")) { return FBlueprintEditOps::FunctionRemoveParam(Params); }
	if (Method == TEXT("edit.function.set_flags"))    { return FBlueprintEditOps::FunctionSetFlags(Params); }
	if (Method == TEXT("edit.function.override"))     { return FBlueprintEditOps::FunctionOverride(Params); }
	if (Method == TEXT("edit.event.add_custom"))      { return FBlueprintEditOps::EventAddCustom(Params); }
	if (Method == TEXT("edit.event.remove"))          { return FBlueprintEditOps::EventRemove(Params); }
	if (Method == TEXT("edit.event.implement"))       { return FBlueprintEditOps::EventImplement(Params); }
	if (Method == TEXT("edit.dispatcher.remove"))     { return FBlueprintEditOps::DispatcherRemove(Params); }

	if (Method == TEXT("edit.external.rewrite_call"))     { return FBlueprintEditOps::ExternalRewriteCall(Params); }
	if (Method == TEXT("edit.external.rewrite_delegate")) { return FBlueprintEditOps::ExternalRewriteDelegate(Params); }

	if (Method == TEXT("edit.component.add"))          { return FBlueprintEditOps::ComponentAdd(Params); }
	if (Method == TEXT("edit.component.remove"))       { return FBlueprintEditOps::ComponentRemove(Params); }
	if (Method == TEXT("edit.component.reparent"))     { return FBlueprintEditOps::ComponentReparent(Params); }
	if (Method == TEXT("edit.component.set_property")) { return FBlueprintEditOps::ComponentSetProperty(Params); }

	// --- Phase D: node graph editing ---

	// Generic ops (BlueprintEditOps_Nodes.cpp)
	if (Method == TEXT("edit.node.remove"))         { return FBlueprintEditOps::NodeRemove(Params); }
	if (Method == TEXT("edit.node.remove_broken")) { return FBlueprintEditOps::NodeRemoveBroken(Params); }
	if (Method == TEXT("edit.node.refresh_variables")) { return FBlueprintEditOps::NodeRefreshVariables(Params); }
	if (Method == TEXT("edit.node.move"))           { return FBlueprintEditOps::NodeMove(Params); }
	if (Method == TEXT("edit.node.add_generic"))    { return FBlueprintEditOps::NodeAddGeneric(Params); }
	if (Method == TEXT("edit.pin.set_default"))     { return FBlueprintEditOps::PinSetDefault(Params); }
	if (Method == TEXT("edit.pin.connect"))         { return FBlueprintEditOps::PinConnect(Params); }
	if (Method == TEXT("edit.pin.disconnect"))      { return FBlueprintEditOps::PinDisconnect(Params); }
	if (Method == TEXT("edit.pin.break_all_links")) { return FBlueprintEditOps::PinBreakAllLinks(Params); }

	// Curated node builders (BlueprintEditOps_NodeBuilders.cpp)
	if (Method == TEXT("edit.node.add_variable_get"))  { return FBlueprintEditOps::NodeAddVariableGet(Params); }
	if (Method == TEXT("edit.node.add_variable_set"))  { return FBlueprintEditOps::NodeAddVariableSet(Params); }
	if (Method == TEXT("edit.node.add_function_call")) { return FBlueprintEditOps::NodeAddFunctionCall(Params); }
	if (Method == TEXT("edit.node.add_branch"))        { return FBlueprintEditOps::NodeAddBranch(Params); }
	if (Method == TEXT("edit.node.add_sequence"))      { return FBlueprintEditOps::NodeAddSequence(Params); }
	if (Method == TEXT("edit.node.add_for_loop"))      { return FBlueprintEditOps::NodeAddForLoop(Params); }
	if (Method == TEXT("edit.node.add_cast"))          { return FBlueprintEditOps::NodeAddCast(Params); }
	if (Method == TEXT("edit.node.add_make_struct"))   { return FBlueprintEditOps::NodeAddMakeStruct(Params); }
	if (Method == TEXT("edit.node.add_break_struct"))  { return FBlueprintEditOps::NodeAddBreakStruct(Params); }
	if (Method == TEXT("edit.node.add_literal"))       { return FBlueprintEditOps::NodeAddLiteral(Params); }

	return FBlueprintEditHelpers::MakeEditError(FString::Printf(TEXT("Unknown edit method: %s"), *Method));
}
