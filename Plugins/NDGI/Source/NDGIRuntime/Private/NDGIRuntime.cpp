// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIRuntime.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY(LogNDGIRuntime);

namespace NDGI
{
	namespace Runtime
	{
		/** Starts the packaged smoke test when -ndgiSmoke is on the command line. */
		void StartRuntimeSmokeFromCommandLine();
	}
}

void FNDGIRuntimeModule::StartupModule()
{
	// Global shader directory mapping must happen at PostConfigInit. This module owns it so
	// packaged builds can resolve /Plugin/NDGI shader paths as well.
	if (IPluginManager::Get().FindPlugin(TEXT("NDGI")))
	{
		FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("NDGI"))->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/NDGI"), PluginShaderDir);
	}

	// Shipping builds compile out -ExecCmds, so the packaged smoke test uses a
	// dedicated command-line switch that is parsed here and started after the
	// engine (and a game world) exists.
	if (FParse::Param(FCommandLine::Get(), TEXT("ndgiSmoke")))
	{
		FCoreDelegates::GetOnPostEngineInit().AddLambda([]()
		{
			NDGI::Runtime::StartRuntimeSmokeFromCommandLine();
		});
	}
}

void FNDGIRuntimeModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FNDGIRuntimeModule, NDGIRuntime)
