// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace NDGI
{
	namespace EditorTools
	{
		/**
		 * Compresses an RGBA8 feature map to BC7 (one byte per texel, 16-byte
		 * 4x4 blocks). Feature channels are stored in R, G, B, A order.
		 *
		 * Uses the engine texture-format provider so the editor and the cooked
		 * runtime agree on the exact BC7 bitstream. Returns false if the BC7
		 * format is unavailable or the image is not block aligned.
		 */
		bool CompressRGBA8ToBC7(
			const TArray<uint8>& RGBA,
			int32 Width,
			int32 Height,
			TArray<uint8>& OutBC7,
			FString& OutError);

		/** Decodes a BC7 blob back to RGBA8 for validation (editor only). */
		bool DecodeBC7ToRGBA8(
			const TArray<uint8>& BC7,
			int32 Width,
			int32 Height,
			TArray<uint8>& OutRGBA,
			FString& OutError);
	}
}
