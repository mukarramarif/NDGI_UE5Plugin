// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIBC7Compress.h"

#include "BatchedElements.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameTime.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "NDGI.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"

namespace
{
	bool ReadbackTextureLinear(UTexture2D* Texture, TArray<FLinearColor>& OutPixels, int32& OutWidth, int32& OutHeight)
	{
		if (!Texture || !Texture->GetResource() || !Texture->GetPlatformData()
			|| Texture->GetPlatformData()->Mips.Num() == 0)
		{
			return false;
		}
		const int32 Width = Texture->GetPlatformData()->Mips[0].SizeX;
		const int32 Height = Texture->GetPlatformData()->Mips[0].SizeY;
		if (Width <= 0 || Height <= 0)
		{
			return false;
		}

		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		RenderTarget->RenderTargetFormat = RTF_RGBA16f;
		RenderTarget->ClearColor = FLinearColor::Black;
		RenderTarget->bAutoGenerateMips = false;
		RenderTarget->InitCustomFormat(Width, Height, PF_FloatRGBA, false);
		RenderTarget->UpdateResourceImmediate(true);
		FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
		if (!Resource)
		{
			return false;
		}

		{
			FCanvas Canvas(Resource, nullptr, FGameTime::GetTimeSinceAppStart(), GMaxRHIFeatureLevel);
			Canvas.Clear(FLinearColor::Black);
			FCanvasTileItem Tile(FVector2D::ZeroVector, Texture->GetResource(), FVector2D(Width, Height), FLinearColor::White);
			Tile.BlendMode = SE_BLEND_Opaque;
			Canvas.DrawItem(Tile);
			Canvas.Flush_GameThread();
		}
		FlushRenderingCommands();

		FRHIGPUTextureReadback* Readback = new FRHIGPUTextureReadback(TEXT("NDGI.TestBC7Readback"));
		FTextureRHIRef TargetRHI = Resource->GetRenderTargetTexture();
		ENQUEUE_RENDER_COMMAND(NDGITestBC7Copy)(
			[Readback, TargetRHI](FRHICommandListImmediate& RHICmdList)
			{
				Readback->EnqueueCopy(RHICmdList, TargetRHI);
			});
		FlushRenderingCommands();

		for (int32 Spin = 0; Spin < 10000 && !Readback->IsReady(); ++Spin)
		{
			FPlatformProcess::Sleep(0.001f);
		}
		if (!Readback->IsReady())
		{
			ENQUEUE_RENDER_COMMAND(NDGITestBC7Delete)([Readback](FRHICommandListImmediate&) { delete Readback; });
			FlushRenderingCommands();
			return false;
		}

		OutWidth = Width;
		OutHeight = Height;
		OutPixels.SetNumUninitialized(Width * Height);
		ENQUEUE_RENDER_COMMAND(NDGITestBC7Lock)(
			[Readback, &OutPixels, Width, Height](FRHICommandListImmediate& RHICmdList)
			{
				int32 RowPitchInPixels = 0;
				int32 BufferHeight = 0;
				const void* Data = Readback->Lock(RowPitchInPixels, &BufferHeight);
				if (Data)
				{
					for (int32 Y = 0; Y < FMath::Min(BufferHeight, Height); ++Y)
					{
						const FFloat16Color* Row = reinterpret_cast<const FFloat16Color*>(
							static_cast<const uint8*>(Data) + static_cast<int64>(Y) * RowPitchInPixels * sizeof(FFloat16Color));
						for (int32 X = 0; X < Width; ++X)
						{
							OutPixels[Y * Width + X] = FLinearColor(Row[X].R, Row[X].G, Row[X].B, Row[X].A);
						}
					}
				}
				Readback->Unlock();
				delete Readback;
			});
		FlushRenderingCommands();
		return true;
	}

	void RunTestBC7(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		bool bQuitWhenDone = false;
		for (const FString& Arg : Args)
		{
			bQuitWhenDone |= Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase);
		}

		constexpr int32 Width = 64;
		constexpr int32 Height = 64;
		TArray<uint8> Source;
		Source.SetNumUninitialized(Width * Height * 4);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const int32 Index = (Y * Width + X) * 4;
				Source[Index + 0] = static_cast<uint8>(FMath::Clamp(X * 4, 0, 255));
				Source[Index + 1] = static_cast<uint8>(FMath::Clamp(Y * 4, 0, 255));
				Source[Index + 2] = static_cast<uint8>(((X / 8 + Y / 8) & 1) ? 210 : 35);
				Source[Index + 3] = static_cast<uint8>(255 - ((X * 3 + Y * 5) & 127));
			}
		}

		TArray<uint8> BC7;
		FString Error;
		if (!NDGI::EditorTools::CompressRGBA8ToBC7(Source, Width, Height, BC7, Error))
		{
			UE_LOG(LogNDGI, Error, TEXT("TestBC7: encode failed: %s"), *Error);
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}

		UTexture2D* Texture = UTexture2D::CreateTransient(
			Width, Height, PF_BC7, TEXT("NDGI.TestBC7"), TConstArrayView64<uint8>(BC7));
		if (Texture)
		{
			Texture->SRGB = false;
			Texture->UpdateResource();
		}
		TArray<FLinearColor> Pixels;
		int32 ReadWidth = 0;
		int32 ReadHeight = 0;
		if (!Texture || !ReadbackTextureLinear(Texture, Pixels, ReadWidth, ReadHeight)
			|| ReadWidth != Width || ReadHeight != Height)
		{
			UE_LOG(LogNDGI, Error, TEXT("TestBC7: GPU readback failed."));
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}

		double DirectSquared = 0.0;
		double SwappedSquared = 0.0;
		double ChannelSquared[4] = { 0.0, 0.0, 0.0, 0.0 };
		int32 MaxError = 0;
		for (int32 Index = 0; Index < Width * Height; ++Index)
		{
			const FLinearColor& Pixel = Pixels[Index];
			const double Decoded[4] =
			{
				Pixel.R * 255.0, Pixel.G * 255.0, Pixel.B * 255.0, Pixel.A * 255.0
			};
			for (int32 Channel = 0; Channel < 4; ++Channel)
			{
				const double A = Source[Index * 4 + Channel];
				const double B = Decoded[Channel];
				const double Diff = A - B;
				DirectSquared += Diff * Diff;
				ChannelSquared[Channel] += Diff * Diff;
				MaxError = FMath::Max(MaxError, FMath::RoundToInt(FMath::Abs(Diff)));
			}
			const double SwappedR = Source[Index * 4 + 0] - Decoded[2];
			const double SwappedB = Source[Index * 4 + 2] - Decoded[0];
			SwappedSquared += SwappedR * SwappedR + SwappedB * SwappedB;
		}
		const double Count = static_cast<double>(Width * Height * 4);
		const double Mse = DirectSquared / Count;
		const double Psnr = Mse > 1e-9 ? 10.0 * FMath::LogX(10.0, (255.0 * 255.0) / Mse) : 99.0;
		const double ChannelMse[4] =
		{
			ChannelSquared[0] / static_cast<double>(Width * Height),
			ChannelSquared[1] / static_cast<double>(Width * Height),
			ChannelSquared[2] / static_cast<double>(Width * Height),
			ChannelSquared[3] / static_cast<double>(Width * Height)
		};
		const double ChannelPsnr[4] =
		{
			ChannelMse[0] > 1e-9 ? 10.0 * FMath::LogX(10.0, (255.0 * 255.0) / ChannelMse[0]) : 99.0,
			ChannelMse[1] > 1e-9 ? 10.0 * FMath::LogX(10.0, (255.0 * 255.0) / ChannelMse[1]) : 99.0,
			ChannelMse[2] > 1e-9 ? 10.0 * FMath::LogX(10.0, (255.0 * 255.0) / ChannelMse[2]) : 99.0,
			ChannelMse[3] > 1e-9 ? 10.0 * FMath::LogX(10.0, (255.0 * 255.0) / ChannelMse[3]) : 99.0
		};

		// A channel-swapped reconstruction must be noticeably worse than the
		// direct one; this catches B/R order mistakes in the BC7 roundtrip.
		const bool bChannelOrderOk = DirectSquared < SwappedSquared * 0.5;
		const bool bPassed = bChannelOrderOk && Psnr >= 25.0 && MaxError <= 96;
		UE_LOG(LogNDGI, Display,
			TEXT("TestBC7: %s - %dx%d, encoded %d bytes, %.2f dB overall, channel PSNR R/G/B/A %.2f/%.2f/%.2f/%.2f, max err %d, direct/swapped MSE %.3f/%.3f."),
			bPassed ? TEXT("PASSED") : TEXT("FAILED"), Width, Height, BC7.Num(), Psnr,
			ChannelPsnr[0], ChannelPsnr[1], ChannelPsnr[2], ChannelPsnr[3], MaxError,
			Mse, SwappedSquared / static_cast<double>(Width * Height * 2));

		if (bQuitWhenDone && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestBC7Command(
	TEXT("ndgi.TestBC7"),
	TEXT("Round-trips a known RGBA8 pattern through the engine BC7 encoder/decoder and checks per-channel PSNR and channel order. Usage: ndgi.TestBC7 [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestBC7));
