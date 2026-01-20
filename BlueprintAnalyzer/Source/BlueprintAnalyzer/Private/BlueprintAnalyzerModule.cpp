// BlueprintAnalyzerModule.cpp
// Module implementation for BlueprintAnalyzer plugin

#include "BlueprintAnalyzerModule.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "FBlueprintAnalyzerModule"

void FBlueprintAnalyzerModule::StartupModule()
{
	// This code will execute after your module is loaded into memory
	// The exact timing is specified in the .uplugin file per-module
	UE_LOG(LogTemp, Log, TEXT("BlueprintAnalyzer module starting up"));
}

void FBlueprintAnalyzerModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module
	UE_LOG(LogTemp, Log, TEXT("BlueprintAnalyzer module shutting down"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBlueprintAnalyzerModule, BlueprintAnalyzer)
