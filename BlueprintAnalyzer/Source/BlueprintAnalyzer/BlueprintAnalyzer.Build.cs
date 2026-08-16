// BlueprintAnalyzer.Build.cs
// Recovered/reconstructed module rules

using UnrealBuildTool;

public class BlueprintAnalyzer : ModuleRules
{
	public BlueprintAnalyzer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
			// FCompositeFont / FTypeface / FFontData for UFont composite-font ops.
			"SlateCore"
		});

		if (Target.Type == TargetType.Editor)
		{
			PublicDependencyModuleNames.AddRange(new string[]
			{
				"UnrealEd",
				"BlueprintGraph",
				"Kismet",
				"AssetRegistry",
				// UMG + UMGEditor for WidgetTree introspection (UWidget, UPanelWidget,
				// UPanelSlot, UWidgetBlueprint).
				"UMG",
				"UMGEditor",
				// IAssetTools::ImportAssetTasks for automated FBX anim import.
				"AssetTools",
				// ISourceControlModule::SetProvider(None) — pipe server keeps
				// asset saves SCC-neutral so tooling owns the p4 lifecycle.
				"SourceControl"
			});
		}
	}
}
