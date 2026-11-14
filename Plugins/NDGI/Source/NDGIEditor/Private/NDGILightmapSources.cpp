// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGILightmapSources.h"

#include "BatchedElements.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameTime.h"
#include "HAL/PlatformProcess.h"
#include "LightMap.h"
#include "NDGI.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "StaticMeshComponentLODInfo.h"

namespace NDGI
{
	namespace Editor
	{
		namespace LightmapSources
		{
			bool ReadbackTexture(UTexture2D* Texture, FReadback& OutResult)
			{
				OutResult = FReadback();
				if (!Texture || !Texture->GetResource())
				{
					return false;
				}
				FTexturePlatformData* PlatformData = Texture->GetPlatformData();
				if (!PlatformData || PlatformData->Mips.Num() == 0)
				{
					return false;
				}
				const int32 Width = PlatformData->Mips[0].SizeX;
				const int32 Height = PlatformData->Mips[0].SizeY;
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

				OutResult.Width = Width;
				OutResult.Height = Height;
				TSharedRef<FReadback> Result = MakeShared<FReadback>();
				Result->Width = Width;
				Result->Height = Height;

				FRHIGPUTextureReadback* Readback = new FRHIGPUTextureReadback(TEXT("NDGI.LightmapInspectorReadback"));
				FTextureRHIRef TargetRHI = Resource->GetRenderTargetTexture();
				ENQUEUE_RENDER_COMMAND(NDGIEnqueueInspectorLightmapCopy)(
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
					UE_LOG(LogNDGI, Warning, TEXT("Lightmap inspector: timed out waiting for texture readback."));
					ENQUEUE_RENDER_COMMAND(NDGIDeleteInspectorLightmapReadback)([Readback](FRHICommandListImmediate&) { delete Readback; });
					FlushRenderingCommands();
					return false;
				}

				ENQUEUE_RENDER_COMMAND(NDGILockInspectorLightmapReadback)(
					[Readback, Result](FRHICommandListImmediate&)
					{
						int32 RowPitchInPixels = 0;
						int32 BufferHeight = 0;
						const void* Data = Readback->Lock(RowPitchInPixels, &BufferHeight);
						if (Data)
						{
							const int32 Height = FMath::Min(BufferHeight, Result->Height);
							Result->Pixels.SetNumZeroed(Result->Width * Result->Height);
							for (int32 Y = 0; Y < Height; ++Y)
							{
								const FFloat16Color* Row = reinterpret_cast<const FFloat16Color*>(
									static_cast<const uint8*>(Data) + static_cast<int64>(Y) * RowPitchInPixels * sizeof(FFloat16Color));
								for (int32 X = 0; X < Result->Width; ++X)
								{
									Result->Pixels[Y * Result->Width + X] = FLinearColor(Row[X].R, Row[X].G, Row[X].B, Row[X].A);
								}
							}
							Result->bSuccess = true;
						}
						Readback->Unlock();
						delete Readback;
					});
				FlushRenderingCommands();

				OutResult = MoveTemp(*Result);
				return OutResult.bSuccess;
			}

			void CollectLightmapTextures(
				UWorld* World,
				TArray<FAtlasSource>& OutSources,
				int32& OutVirtualTextureCount)
			{
				OutVirtualTextureCount = 0;
				OutSources.Reset();
				if (!World)
				{
					return;
				}

				TMap<UTexture2D*, FAtlasSource> UniqueSources;
				for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
				{
					UStaticMeshComponent* Component = *It;
					if (!Component || Component->GetWorld() != World)
					{
						continue;
					}
					if (Component->Mobility != EComponentMobility::Static || Component->LODData.Num() == 0)
					{
						continue;
					}

					// Read map build data directly: FStaticLightingSystemInterface prefers
					// GPULightmass when enabled, which returns nothing for CPU bakes.
					const FMeshMapBuildData* BuildData = Component->GetMeshMapBuildData(Component->LODData[0]);
					if (!BuildData || !BuildData->LightMap)
					{
						continue;
					}
					const FLightMap2D* LightMap2D = BuildData->LightMap->GetLightMap2D();
					if (!LightMap2D)
					{
						continue;
					}

					const bool bHighQuality = LightMap2D->IsValid(0);
					const int32 BasisIndex = bHighQuality ? 0 : 1;
					if (!LightMap2D->IsValid(BasisIndex))
					{
						if (LightMap2D->IsVirtualTextureValid())
						{
							++OutVirtualTextureCount;
						}
						continue;
					}
					UTexture2D* Texture = const_cast<UTexture2D*>(LightMap2D->GetTexture(BasisIndex));
					if (!Texture || UniqueSources.Contains(Texture))
					{
						continue;
					}

					FAtlasSource Source;
					Source.Texture = Texture;
					Source.bHighQuality = bHighQuality;
					const int32 CoefIndex = bHighQuality ? 0 : LQ_LIGHTMAP_COEF_INDEX;
					Source.Scale = LightMap2D->ScaleVectors[CoefIndex];
					Source.Add = LightMap2D->AddVectors[CoefIndex];
					Source.ScaleSH = LightMap2D->ScaleVectors[1];
					Source.AddSH = LightMap2D->AddVectors[1];
					UniqueSources.Add(Texture, Source);
				}

				UniqueSources.GenerateValueArray(OutSources);
				OutSources.Sort([](const FAtlasSource& A, const FAtlasSource& B)
				{
					return A.Texture->GetPathName() < B.Texture->GetPathName();
				});
			}

			void DecodeHQTexel(
				const FLinearColor& Top,
				const FLinearColor& Bottom,
				const FVector4f& Scale0,
				const FVector4f& Add0,
				const FVector4f& Scale1,
				const FVector4f& Add1,
				FVector3f& OutRGB,
				FVector3f& OutSH)
			{
				float LogL = Top.A + Bottom.A * (1.0f / 255.0f) - (0.5f / 255.0f);
				LogL = LogL * Scale0.W + Add0.W;
				const float L = FMath::Exp2(LogL) - 0.01858136f;
				const FVector3f UVW = FVector3f(Top.R * Top.R, Top.G * Top.G, Top.B * Top.B)
					* FVector3f(Scale0.X, Scale0.Y, Scale0.Z)
					+ FVector3f(Add0.X, Add0.Y, Add0.Z);
				OutRGB = UVW * FMath::Max(L, 0.0f);
				OutSH = FVector3f(Bottom.R, Bottom.G, Bottom.B)
					* FVector3f(Scale1.X, Scale1.Y, Scale1.Z)
					+ FVector3f(Add1.X, Add1.Y, Add1.Z);
			}

			void DecodeLQTexel(
				const FLinearColor& Top,
				const FVector4f& Scale0,
				const FVector4f& Add0,
				FVector3f& OutRGB,
				FVector3f& OutSH)
			{
				const FVector3f LogRGB = FVector3f(Top.R, Top.G, Top.B)
					* FVector3f(Scale0.X, Scale0.Y, Scale0.Z)
					+ FVector3f(Add0.X, Add0.Y, Add0.Z);
				const float LogL = 0.3f * LogRGB.X + 0.59f * LogRGB.Y + 0.11f * LogRGB.Z;
				const float L = FMath::Exp2(LogL * 16.0f - 8.0f) - (1.0f / 256.0f);
				OutRGB = LogRGB * (FMath::Max(L, 0.0f) / FMath::Max(LogL, 1e-5f));
				OutSH = FVector3f::ZeroVector;
			}
		}
	}
}
