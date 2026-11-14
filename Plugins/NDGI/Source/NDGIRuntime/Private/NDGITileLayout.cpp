// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGITileLayout.h"

namespace NDGI
{
	namespace Train
	{
		FTileLayout ComputeTileLayout(const FTileTrainConfig& Config)
		{
			FTileLayout Layout;
			Layout.UVBlocksX = static_cast<uint32>(Config.UVSize / 4);
			Layout.UVTBlocksX = static_cast<uint32>(Config.UVTSize / 4);
			const uint32 UVBlocks = Layout.UVBlocksX * Layout.UVBlocksX;
			const uint32 UVTBlocksTotal = Layout.UVTBlocksX * Layout.UVTBlocksX * static_cast<uint32>(Config.TimeSlices);

			uint32 Offset = 0;
			Layout.MLPOffset = Offset; Offset += MLPParameterCount;
			Layout.UVE1Offset = Offset; Offset += UVBlocks * 4;
			Layout.UVE2Offset = Offset; Offset += UVBlocks * 4;
			Layout.UVWOffset = Offset; Offset += UVBlocks * 16;
			Layout.UVTE1Offset = Offset; Offset += UVTBlocksTotal * 4;
			Layout.UVTE2Offset = Offset; Offset += UVTBlocksTotal * 4;
			Layout.UVTWOffset = Offset; Offset += UVTBlocksTotal * 16;
			Layout.UTOffset = Offset; Offset += static_cast<uint32>(Config.UTSize * Config.TimeSlices * 2);
			Layout.VTOffset = Offset; Offset += static_cast<uint32>(Config.VTSize * Config.TimeSlices * 2);
			Layout.TotalParams = Offset;
			return Layout;
		}

		int32 ComputeTotalParameters(const FTileTrainConfig& Config)
		{
			return static_cast<int32>(ComputeTileLayout(Config).TotalParams);
		}
	}
}
