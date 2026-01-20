// BlueprintAnalyzerModule.h
// Module interface for BlueprintAnalyzer plugin

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FBlueprintAnalyzerModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Singleton-like access to this module's interface.
	 *
	 * @return Returns singleton instance, loading the module on demand if needed
	 */
	static inline FBlueprintAnalyzerModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FBlueprintAnalyzerModule>("BlueprintAnalyzer");
	}

	/**
	 * Checks to see if this module is loaded and ready.
	 *
	 * @return True if the module is loaded and ready to use
	 */
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("BlueprintAnalyzer");
	}
};
