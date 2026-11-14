// Copyright Epic Games, Inc. All Rights Reserved.

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Math/RandomStream.h"
#include "Misc/PackageName.h"
#include "NDGI.h"
#include "NDGIModelExport.h"
#include "NDGIReference.h"
#include "NDGIModelAsset.h"
#include "NDGITestGroundTruth.h"
#include "NDGITileReference.h"
#include "NDGITileTrain.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	using namespace NDGI;

	void RunExportTile(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		bool bQuitWhenDone = false;
		int32 TrainSteps = 2000;
		int32 NumericArgIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				if (NumericArgIndex == 0) { TrainSteps = FMath::Max(0, FCString::Atoi(*Arg)); }
				++NumericArgIndex;
			}
		}

		auto Finish = [bQuitWhenDone]()
		{
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		Reference::FTileConfig Config;
		Config.UVSize = 64;
		Config.UVTSize = 16;
		Config.UTSize = 32;
		Config.VTSize = 32;
		Config.TimeSlices = 8;
		Config.bSimulateBC = true;
		Reference::FTileReference Tile(Config, 1337);

		// Train the demo tile on the synthetic ground truth so the preview shows a learned
		// result rather than random feature maps.
		if (TrainSteps > 0)
		{
			using TestUtil::FGroundTruth;
			using TestUtil::GenerateSamples;
			const FGroundTruth GroundTruth = FGroundTruth::Make(1337);
			const TArray<Reference::FTrainSample> TrainPool = GenerateSamples(GroundTruth, 1337 ^ 0x11111111, TrainSteps * 64);
			const TArray<Reference::FTrainSample> EvalPool = GenerateSamples(GroundTruth, 1337 ^ 0x22222222, 2048);

			FRandomStream NoiseStream(1337 ^ 0x9e3779b9);
			double LastLoss = 0.0;
			for (int32 Step = 0; Step < TrainSteps; ++Step)
			{
				const TConstArrayView<Reference::FTrainSample> BatchView(TrainPool.GetData() + static_cast<int64>(Step) * 64, 64);
				Tile.TrainStep(BatchView, NoiseStream, LastLoss);
			}
			double Mse = 0.0;
			double Psnr = 0.0;
			Tile.Evaluate(EvalPool, Mse, Psnr);
			UE_LOG(LogNDGI, Display, TEXT("ExportTile: trained %d steps, train loss %.3e, eval PSNR %.2f dB."), TrainSteps, LastLoss, Psnr);
		}

		// Finalize, reconstruct, and export the model asset (reload-verified).
		const FString PackageName = TEXT("/Game/NDGI/TestModel");
		const FString AssetName = TEXT("TestModel");
		// The demo tile is trained on raw synthetic targets (no gamma/mean preprocessing),
		// so the model stores identity denormalization.
		FString Error;
		if (!EditorTools::WriteModelAsset(PackageName, AssetName, Tile, Config, TArray<float>(), TArray<float>(), 1.0f, Error))
		{
			UE_LOG(LogNDGI, Error, TEXT("ExportTile: %s"), *Error);
			Finish();
			return;
		}

		const FString FileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		UE_LOG(LogNDGI, Display,
			TEXT("ExportTile: PASSED (F_uv %dx%dx4, F_uvt %dx%dx%d x4, MLP %d floats, file %.1f KB)."),
			Config.UVSize, Config.UVSize, Config.UVTSize, Config.UVTSize, Config.TimeSlices, Train::MLPParameterCount,
			IFileManager::Get().FileSize(*FileName) / 1024.0f);

		Finish();
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIExportTileCommand(
	TEXT("ndgi.ExportTile"),
	TEXT("Finalizes a reference tile and exports it as a model data asset, then reloads and verifies it. Optional arg: quit."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunExportTile));
