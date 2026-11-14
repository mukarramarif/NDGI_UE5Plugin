// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class NDGIEditor : ModuleRules
{
	public NDGIEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"ApplicationCore",
				"RenderCore",
				"RHI",
				"Projects",
				"UnrealEd",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"MaterialEditor",
				"GPULightmass",
				"NDGIRuntime",
				"NDGI",
				// BC7 encoding of the trained feature maps. TextureFormat provides
				// the platform BC7 encoder (Oodle/ISPC) and TextureCompressor owns
				// the shared build settings used by that API.
				"TextureCompressor",
				"TextureFormat",
				"ImageCore",
			}
			);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
			);
	}
}
