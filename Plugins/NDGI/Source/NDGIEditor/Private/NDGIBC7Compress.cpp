// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIBC7Compress.h"

#include "ImageCore.h"
#include "Interfaces/ITextureFormat.h"
#include "Memory/SharedBuffer.h"
#include "Interfaces/ITextureFormatManagerModule.h"
#include "TextureCompressorModule.h"
#include "TextureFormatManager.h"

namespace NDGI
{
	namespace EditorTools
	{
		bool CompressRGBA8ToBC7(
			const TArray<uint8>& RGBA,
			int32 Width,
			int32 Height,
			TArray<uint8>& OutBC7,
			FString& OutError)
		{
			OutBC7.Reset();

			if (Width <= 0 || Height <= 0 || Width % 4 != 0 || Height % 4 != 0)
			{
				OutError = FString::Printf(TEXT("BC7 requires positive, 4x4-aligned dimensions (got %dx%d)."), Width, Height);
				return false;
			}
			if (RGBA.Num() != Width * Height * 4)
			{
				OutError = FString::Printf(TEXT("Feature buffer has %d bytes, expected %d for %dx%d RGBA8."),
					RGBA.Num(), Width * Height * 4, Width, Height);
				return false;
			}

			ITextureFormatManagerModule* FormatManager = GetTextureFormatManager();
			if (!FormatManager)
			{
				OutError = TEXT("TextureFormat module is unavailable; cannot encode BC7.");
				return false;
			}

			const ITextureFormat* BC7Format = FormatManager->FindTextureFormat(FName(TEXT("BC7")));
			if (!BC7Format)
			{
				OutError = TEXT("No BC7 texture format provider is loaded; enable the Oodle/ISPC texture format plugin for this platform.");
				return false;
			}

			// FImage's 8-bit format is BGRA8 in memory, but FColor carries semantic
			// R/G/B/A fields. The texture-format provider keeps those semantics in
			// the BC7 result, so sampling returns the feature channels in R,G,B,A.
			FImage Image(Width, Height, 1, ERawImageFormat::BGRA8, EGammaSpace::Linear);
			TArrayView64<FColor> Pixels = Image.AsBGRA8();
			for (int32 Index = 0; Index < Width * Height; ++Index)
			{
				FColor& Pixel = Pixels[Index];
				Pixel.R = RGBA[Index * 4 + 0];
				Pixel.G = RGBA[Index * 4 + 1];
				Pixel.B = RGBA[Index * 4 + 2];
				Pixel.A = RGBA[Index * 4 + 3];
			}

			FTextureBuildSettings Settings;
			Settings.TextureFormatName = FName(TEXT("BC7"));
			Settings.bSRGB = false;
			Settings.bHDRSource = false;
			Settings.bTextureArray = false;
			Settings.bVolume = false;
			Settings.bCubemap = false;
			Settings.bForceAlphaChannel = true;
			Settings.bForceNoAlphaChannel = false;
			Settings.bVirtualStreamable = false;

			FCompressedImage2D Compressed;
			const FIntVector3 Mip0Dimensions(Width, Height, 1);
			const bool bCompressed = BC7Format->CompressImage(
				Image,
				Settings,
				Mip0Dimensions,
				/*InMip0NumSlicesNoDepth=*/1,
				/*InMipIndex=*/0,
				/*InMipCount=*/1,
				TEXT("NDGI.FeatureBC7"),
				/*bImageHasAlphaChannel=*/true,
				Compressed);

			if (!bCompressed || Compressed.RawData.Num() == 0)
			{
				OutError = FString::Printf(TEXT("BC7 encoder failed for %dx%d feature map."), Width, Height);
				return false;
			}

			// BC7 is 16 bytes per 4x4 block. The provider should return exactly that
			// for the top mip; reject anything else instead of uploading a malformed
			// texture array.
			const int64 ExpectedBytes = static_cast<int64>(Width / 4) * static_cast<int64>(Height / 4) * 16;
			if (Compressed.RawData.Num() != ExpectedBytes)
			{
				OutError = FString::Printf(TEXT("BC7 encoder returned %lld bytes, expected %lld for %dx%d."),
					Compressed.RawData.Num(), ExpectedBytes, Width, Height);
				return false;
			}

			OutBC7.SetNumUninitialized(static_cast<int32>(Compressed.RawData.Num()));
			FMemory::Memcpy(OutBC7.GetData(), Compressed.RawData.GetData(), Compressed.RawData.Num());
			return true;
		}

		bool DecodeBC7ToRGBA8(
			const TArray<uint8>& BC7,
			int32 Width,
			int32 Height,
			TArray<uint8>& OutRGBA,
			FString& OutError)
		{
			OutRGBA.Reset();
			if (Width <= 0 || Height <= 0 || Width % 4 != 0 || Height % 4 != 0)
			{
				OutError = FString::Printf(TEXT("BC7 decode requires 4x4-aligned dimensions (got %dx%d)."), Width, Height);
				return false;
			}
			const int64 ExpectedBytes = static_cast<int64>(Width / 4) * static_cast<int64>(Height / 4) * 16;
			if (BC7.Num() != ExpectedBytes)
			{
				OutError = FString::Printf(TEXT("BC7 blob has %d bytes, expected %lld for %dx%d."),
					BC7.Num(), ExpectedBytes, Width, Height);
				return false;
			}

			ITextureFormatManagerModule* FormatManager = GetTextureFormatManager();
			const ITextureFormat* BC7Format = FormatManager ? FormatManager->FindTextureFormat(FName(TEXT("BC7"))) : nullptr;
			if (!BC7Format)
			{
				OutError = TEXT("No BC7 texture format provider is loaded.");
				return false;
			}

			FImage DecodedImage;
			const FSharedBuffer Encoded = FSharedBuffer::Clone(BC7.GetData(), BC7.Num());
			if (!BC7Format->DecodeImage(Width, Height, 1, PF_BC7, false, FName(TEXT("BC7")),
				Encoded, DecodedImage, TEXT("NDGI.FeatureBC7")))
			{
				OutError = TEXT("BC7 decode failed.");
				return false;
			}
			if (DecodedImage.Format != ERawImageFormat::BGRA8)
			{
				FImage Converted;
				DecodedImage.CopyTo(Converted, ERawImageFormat::BGRA8, EGammaSpace::Linear);
				DecodedImage = MoveTemp(Converted);
			}

			const TArrayView64<const FColor> Pixels = DecodedImage.AsBGRA8();
			OutRGBA.SetNumUninitialized(Width * Height * 4);
			for (int32 Index = 0; Index < Width * Height; ++Index)
			{
				OutRGBA[Index * 4 + 0] = Pixels[Index].R;
				OutRGBA[Index * 4 + 1] = Pixels[Index].G;
				OutRGBA[Index * 4 + 2] = Pixels[Index].B;
				OutRGBA[Index * 4 + 3] = Pixels[Index].A;
			}
			return true;
		}
	}
}
