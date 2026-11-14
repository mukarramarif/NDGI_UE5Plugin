// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGITileReference.h"

namespace NDGI
{
	namespace Reference
	{
		namespace
		{
			constexpr double WeightEpsilon = 1e-12;

			/** Mirrored (reflect-101) texel addressing so tiles match at their borders. */
			int32 MirrorIndex(int32 Index, int32 Size)
			{
				if (Size <= 1)
				{
					return 0;
				}
				if (Index < 0)
				{
					return -Index;
				}
				if (Index >= Size)
				{
					return 2 * Size - 2 - Index;
				}
				return Index;
			}

			/** Bilinear tap setup for texel-center sampling: centers at (i + 0.5) / Size. */
			void ComputeBilinear(double Coordinate, int32 Size, int32& OutX0, int32& OutX1, double& OutW0, double& OutW1)
			{
				const double C = Coordinate * Size - 0.5;
				const int32 FloorC = static_cast<int32>(FMath::FloorToDouble(C));
				const double F = C - static_cast<double>(FloorC);
				OutX0 = MirrorIndex(FloorC, Size);
				OutX1 = MirrorIndex(FloorC + 1, Size);
				OutW0 = 1.0 - F;
				OutW1 = F;
			}

			/** Temporal tap setup for maps sampled at integer time indices: coordinate = Time * (T - 1). */
			void ComputeTemporal(double Time, int32 TimeSlices, int32& OutT0, int32& OutT1, double& OutW0, double& OutW1)
			{
				const double C = Time * static_cast<double>(TimeSlices - 1);
				const int32 FloorC = static_cast<int32>(FMath::FloorToDouble(C));
				const double F = C - static_cast<double>(FloorC);
				OutT0 = FMath::Clamp(FloorC, 0, TimeSlices - 1);
				OutT1 = FMath::Clamp(FloorC + 1, 0, TimeSlices - 1);
				OutW0 = 1.0 - F;
				OutW1 = F;
			}

			void Quantize8Bit(double& Value)
			{
				Value = FMath::RoundToDouble(Value * 255.0) / 255.0;
			}

			void Quantize8Bit(TArray<double>& Values)
			{
				for (double& Value : Values)
				{
					Quantize8Bit(Value);
				}
			}
		}

		// ---------------------------------------------------------------------------
		// FBCMap
		// ---------------------------------------------------------------------------

		void FBCMap::Resize(int32 InSizeX, int32 InSizeY, int32 InChannels)
		{
			SizeX = InSizeX;
			SizeY = InSizeY;
			Channels = InChannels;
			BlocksX = FMath::DivideAndRoundUp(SizeX, 4);
			BlocksY = FMath::DivideAndRoundUp(SizeY, 4);

			const int32 NumBlocks = BlocksX * BlocksY;
			E1.SetNumZeroed(NumBlocks * Channels);
			E2.SetNumZeroed(NumBlocks * Channels);
			W.SetNumZeroed(NumBlocks * 16);
			TexelGrad.SetNumZeroed(SizeX * SizeY * Channels);
			DE1.SetNumZeroed(NumBlocks * Channels);
			DE2.SetNumZeroed(NumBlocks * Channels);
			DW.SetNumZeroed(NumBlocks * 16);
		}

		int32 FBCMap::BlockIndex(int32 X, int32 Y) const
		{
			return (Y / 4) * BlocksX + (X / 4);
		}

		int32 FBCMap::TexelIndex(int32 X, int32 Y, int32 Channel) const
		{
			return (Y * SizeX + X) * Channels + Channel;
		}

		double FBCMap::GetTexel(int32 X, int32 Y, int32 Channel) const
		{
			const int32 Block = BlockIndex(X, Y);
			const int32 InBlock = (Y % 4) * 4 + (X % 4);
			const double Weight = W[Block * 16 + InBlock];
			return (1.0 - Weight) * E1[Block * Channels + Channel] + Weight * E2[Block * Channels + Channel];
		}

		void FBCMap::ZeroGradients()
		{
			FMemory::Memzero(TexelGrad.GetData(), TexelGrad.Num() * sizeof(double));
			FMemory::Memzero(DE1.GetData(), DE1.Num() * sizeof(double));
			FMemory::Memzero(DE2.GetData(), DE2.Num() * sizeof(double));
			FMemory::Memzero(DW.GetData(), DW.Num() * sizeof(double));
		}

		void FBCMap::ScatterTexelGradient(int32 X, int32 Y, int32 Channel, double Grad)
		{
			TexelGrad[TexelIndex(X, Y, Channel)] += Grad;
		}

		void FBCMap::Backward()
		{
			const int32 NumBlocks = BlocksX * BlocksY;
			for (int32 Block = 0; Block < NumBlocks; ++Block)
			{
				const int32 BlockX = (Block % BlocksX) * 4;
				const int32 BlockY = (Block / BlocksX) * 4;
				for (int32 P = 0; P < 16; ++P)
				{
					const int32 X = BlockX + (P % 4);
					const int32 Y = BlockY + (P / 4);
					const double Weight = W[Block * 16 + P];
					double DTexel = 0.0;
					for (int32 Channel = 0; Channel < Channels; ++Channel)
					{
						const double D = TexelGrad[TexelIndex(X, Y, Channel)];
						DTexel += D * (E2[Block * Channels + Channel] - E1[Block * Channels + Channel]);
						DE1[Block * Channels + Channel] += (1.0 - Weight) * D;
						DE2[Block * Channels + Channel] += Weight * D;
					}
					DW[Block * 16 + P] += DTexel;
				}
			}
		}

		// ---------------------------------------------------------------------------
		// FTileReference
		// ---------------------------------------------------------------------------

		FTileReference::FTileReference(const FTileConfig& InConfig, int32 Seed)
			: Config(InConfig)
		{
			check(Config.UVSize % 4 == 0 && Config.UVTSize % 4 == 0);
			MLP.Initialize(Seed);
			InitializeStorage(Seed);
		}

		void FTileReference::InitializeStorage(int32 Seed)
		{
			FRandomStream Stream(Seed ^ 0x27d4eb2f);

			auto FillRandom = [&Stream](TArray<double>& Values)
			{
				for (double& Value : Values)
				{
					Value = static_cast<double>(Stream.FRand());
				}
			};

			// F_uv
			UVMap.Resize(Config.UVSize, Config.UVSize, 4);
			FillRandom(UVMap.E1);
			FillRandom(UVMap.E2);
			FillRandom(UVMap.W);
			UVE1Group.Init(UVMap.E1.Num(), Config.LearningRate);
			UVE2Group.Init(UVMap.E2.Num(), Config.LearningRate);
			UVWGroup.Init(UVMap.W.Num(), Config.LearningRate);
			FMemory::Memcpy(UVE1Group.Values.GetData(), UVMap.E1.GetData(), UVMap.E1.Num() * sizeof(double));
			FMemory::Memcpy(UVE2Group.Values.GetData(), UVMap.E2.GetData(), UVMap.E2.Num() * sizeof(double));
			FMemory::Memcpy(UVWGroup.Values.GetData(), UVMap.W.GetData(), UVMap.W.Num() * sizeof(double));

			UVRawGroup.Init(Config.UVSize * Config.UVSize * 4, Config.LearningRate);
			FillRandom(UVRawGroup.Values);

			// F_uvt (per time slice)
			UVTMaps.SetNum(Config.TimeSlices);
			int32 SliceBlocks = 0;
			for (FBCMap& Map : UVTMaps)
			{
				Map.Resize(Config.UVTSize, Config.UVTSize, 4);
				FillRandom(Map.E1);
				FillRandom(Map.E2);
				FillRandom(Map.W);
				SliceBlocks = Map.NumBlocks();
			}
			UVTE1Group.Init(Config.TimeSlices * SliceBlocks * 4, Config.LearningRate);
			UVTE2Group.Init(Config.TimeSlices * SliceBlocks * 4, Config.LearningRate);
			UVTWGroup.Init(Config.TimeSlices * SliceBlocks * 16, Config.LearningRate);
			for (int32 Slice = 0; Slice < UVTMaps.Num(); ++Slice)
			{
				const FBCMap& Map = UVTMaps[Slice];
				FMemory::Memcpy(&UVTE1Group.Values[Slice * SliceBlocks * 4], Map.E1.GetData(), Map.E1.Num() * sizeof(double));
				FMemory::Memcpy(&UVTE2Group.Values[Slice * SliceBlocks * 4], Map.E2.GetData(), Map.E2.Num() * sizeof(double));
				FMemory::Memcpy(&UVTWGroup.Values[Slice * SliceBlocks * 16], Map.W.GetData(), Map.W.Num() * sizeof(double));
			}

			UVTRawGroup.Init(Config.UVTSize * Config.UVTSize * Config.TimeSlices * 4, Config.LearningRate);
			FillRandom(UVTRawGroup.Values);

			// F_ut / F_vt
			UTGroup.Init(Config.UTSize * Config.TimeSlices * 2, Config.LearningRate);
			FillRandom(UTGroup.Values);
			VTGroup.Init(Config.VTSize * Config.TimeSlices * 2, Config.LearningRate);
			FillRandom(VTGroup.Values);

			// MLP
			MLPGroup.Init(NumMLPParameters, Config.LearningRate);
			FlattenParameters(MLP, MLPGroup.Values);
		}

		int32 FTileReference::GetNumTrainableParameters() const
		{
			int32 Total = MLPGroup.Values.Num() + UTGroup.Values.Num() + VTGroup.Values.Num();
			if (Config.bSimulateBC)
			{
				Total += UVE1Group.Values.Num() + UVE2Group.Values.Num() + UVWGroup.Values.Num();
				Total += UVTE1Group.Values.Num() + UVTE2Group.Values.Num() + UVTWGroup.Values.Num();
			}
			else
			{
				Total += UVRawGroup.Values.Num() + UVTRawGroup.Values.Num();
			}
			return Total;
		}

		void FTileReference::SampleUV(double U, double V, double OutValues[4], FSampleInterpolation& Interpolation) const
		{
			ComputeBilinear(U, Config.UVSize, Interpolation.UVX0, Interpolation.UVX1, Interpolation.UVW00, Interpolation.UVW10);
			ComputeBilinear(V, Config.UVSize, Interpolation.UVY0, Interpolation.UVY1, Interpolation.UVW01, Interpolation.UVW11);

			auto Texel = [this](int32 X, int32 Y, int32 Channel) -> double
			{
				if (UsesBCSampling())
				{
					return UVMap.GetTexel(X, Y, Channel);
				}
				const double Raw = UVRawGroup.Values[(Y * Config.UVSize + X) * 4 + Channel];
				return FMath::Clamp(Raw, 0.0, 1.0);
			};

			for (int32 Channel = 0; Channel < 4; ++Channel)
			{
				OutValues[Channel] =
					Interpolation.UVW00 * Texel(Interpolation.UVX0, Interpolation.UVY0, Channel)
					+ Interpolation.UVW10 * Texel(Interpolation.UVX1, Interpolation.UVY0, Channel)
					+ Interpolation.UVW01 * Texel(Interpolation.UVX0, Interpolation.UVY1, Channel)
					+ Interpolation.UVW11 * Texel(Interpolation.UVX1, Interpolation.UVY1, Channel);
			}
		}

		void FTileReference::SampleUT(double U, double Time, double OutValues[2], FSampleInterpolation& Interpolation) const
		{
			ComputeBilinear(U, Config.UTSize, Interpolation.UTX0, Interpolation.UTX1, Interpolation.UTW00, Interpolation.UTW10);
			ComputeTemporal(Time, Config.TimeSlices, Interpolation.UTT0, Interpolation.UTT1, Interpolation.UTW01, Interpolation.UTW11);

			auto Texel = [this](int32 X, int32 T, int32 Channel) -> double
			{
				const double Raw = UTGroup.Values[(T * Config.UTSize + X) * 2 + Channel];
				return FMath::Clamp(Raw, 0.0, 1.0);
			};

			for (int32 Channel = 0; Channel < 2; ++Channel)
			{
				OutValues[Channel] =
					Interpolation.UTW00 * Texel(Interpolation.UTX0, Interpolation.UTT0, Channel)
					+ Interpolation.UTW10 * Texel(Interpolation.UTX1, Interpolation.UTT0, Channel)
					+ Interpolation.UTW01 * Texel(Interpolation.UTX0, Interpolation.UTT1, Channel)
					+ Interpolation.UTW11 * Texel(Interpolation.UTX1, Interpolation.UTT1, Channel);
			}
		}

		void FTileReference::SampleVT(double V, double Time, double OutValues[2], FSampleInterpolation& Interpolation) const
		{
			ComputeBilinear(V, Config.VTSize, Interpolation.VTX0, Interpolation.VTX1, Interpolation.VTW00, Interpolation.VTW10);
			ComputeTemporal(Time, Config.TimeSlices, Interpolation.VTT0, Interpolation.VTT1, Interpolation.VTW01, Interpolation.VTW11);

			auto Texel = [this](int32 X, int32 T, int32 Channel) -> double
			{
				const double Raw = VTGroup.Values[(T * Config.VTSize + X) * 2 + Channel];
				return FMath::Clamp(Raw, 0.0, 1.0);
			};

			for (int32 Channel = 0; Channel < 2; ++Channel)
			{
				OutValues[Channel] =
					Interpolation.VTW00 * Texel(Interpolation.VTX0, Interpolation.VTT0, Channel)
					+ Interpolation.VTW10 * Texel(Interpolation.VTX1, Interpolation.VTT0, Channel)
					+ Interpolation.VTW01 * Texel(Interpolation.VTX0, Interpolation.VTT1, Channel)
					+ Interpolation.VTW11 * Texel(Interpolation.VTX1, Interpolation.VTT1, Channel);
			}
		}

		void FTileReference::SampleUVT(double U, double V, double Time, double OutValues[4], FSampleInterpolation& Interpolation) const
		{
			int32 UX0, UX1, UY0, UY1;
			double WX0, WX1, WY0, WY1;
			ComputeBilinear(U, Config.UVTSize, UX0, UX1, WX0, WX1);
			ComputeBilinear(V, Config.UVTSize, UY0, UY1, WY0, WY1);
			ComputeTemporal(Time, Config.TimeSlices, Interpolation.UVTT0, Interpolation.UVTT1,
				Interpolation.UVTW001, Interpolation.UVTW111); // reuse fields as temporal weights; XY set below

			Interpolation.UVTX0 = UX0;
			Interpolation.UVTX1 = UX1;
			Interpolation.UVTY0 = UY0;
			Interpolation.UVTY1 = UY1;
			Interpolation.UVTW000 = WX0 * WY0;
			Interpolation.UVTW100 = WX1 * WY0;
			Interpolation.UVTW010 = WX0 * WY1;
			Interpolation.UVTW110 = WX1 * WY1;

			const double WeightT0 = Interpolation.UVTW001; // temporal weights captured above
			const double WeightT1 = Interpolation.UVTW111;

			auto Texel = [this](int32 Slice, int32 X, int32 Y, int32 Channel) -> double
			{
				if (UsesBCSampling())
				{
					return UVTMaps[Slice].GetTexel(X, Y, Channel);
				}
				const double Raw = UVTRawGroup.Values[((Slice * Config.UVTSize + Y) * Config.UVTSize + X) * 4 + Channel];
				return FMath::Clamp(Raw, 0.0, 1.0);
			};

			for (int32 Channel = 0; Channel < 4; ++Channel)
			{
				const double BilinearT0 =
					Interpolation.UVTW000 * Texel(Interpolation.UVTT0, UX0, UY0, Channel)
					+ Interpolation.UVTW100 * Texel(Interpolation.UVTT0, UX1, UY0, Channel)
					+ Interpolation.UVTW010 * Texel(Interpolation.UVTT0, UX0, UY1, Channel)
					+ Interpolation.UVTW110 * Texel(Interpolation.UVTT0, UX1, UY1, Channel);
				const double BilinearT1 =
					Interpolation.UVTW000 * Texel(Interpolation.UVTT1, UX0, UY0, Channel)
					+ Interpolation.UVTW100 * Texel(Interpolation.UVTT1, UX1, UY0, Channel)
					+ Interpolation.UVTW010 * Texel(Interpolation.UVTT1, UX0, UY1, Channel)
					+ Interpolation.UVTW110 * Texel(Interpolation.UVTT1, UX1, UY1, Channel);
				OutValues[Channel] = WeightT0 * BilinearT0 + WeightT1 * BilinearT1;
			}
		}

		void FTileReference::Sample(double U, double V, double Time, const double* Noise12, double OutInput[InputDim], FSampleInterpolation& OutInterpolation) const
		{
			double V_uv[4];
			double V_ut[2];
			double V_vt[2];
			double V_uvt[4];

			SampleUV(U, V, V_uv, OutInterpolation);
			SampleUT(U, Time, V_ut, OutInterpolation);
			SampleVT(V, Time, V_vt, OutInterpolation);
			SampleUVT(U, V, Time, V_uvt, OutInterpolation);

			for (int32 Index = 0; Index < 4; ++Index)
			{
				OutInput[Index] = V_uv[Index];
				OutInput[8 + Index] = V_uvt[Index];
			}
			OutInput[4] = V_ut[0];
			OutInput[5] = V_ut[1];
			OutInput[6] = V_vt[0];
			OutInput[7] = V_vt[1];
			EncodeTime(Time, OutInput + 12);

			if (Noise12)
			{
				for (int32 Index = 0; Index < 12; ++Index)
				{
					OutInput[Index] += Noise12[Index];
				}
			}
		}

		void FTileReference::ScatterInputGradient(const FSampleInterpolation& Interpolation, const double InputGrad[InputDim])
		{
			// F_uv (input 0..3)
			for (int32 Channel = 0; Channel < 4; ++Channel)
			{
				const double D = InputGrad[Channel];
				if (UsesBCSampling())
				{
					UVMap.ScatterTexelGradient(Interpolation.UVX0, Interpolation.UVY0, Channel, Interpolation.UVW00 * D);
					UVMap.ScatterTexelGradient(Interpolation.UVX1, Interpolation.UVY0, Channel, Interpolation.UVW10 * D);
					UVMap.ScatterTexelGradient(Interpolation.UVX0, Interpolation.UVY1, Channel, Interpolation.UVW01 * D);
					UVMap.ScatterTexelGradient(Interpolation.UVX1, Interpolation.UVY1, Channel, Interpolation.UVW11 * D);
				}
				else
				{
					auto Accumulate = [this, Channel](int32 X, int32 Y, double Weight, double Grad)
					{
						UVRawGroup.Gradients[(Y * Config.UVSize + X) * 4 + Channel] += Weight * Grad;
					};
					Accumulate(Interpolation.UVX0, Interpolation.UVY0, Interpolation.UVW00, D);
					Accumulate(Interpolation.UVX1, Interpolation.UVY0, Interpolation.UVW10, D);
					Accumulate(Interpolation.UVX0, Interpolation.UVY1, Interpolation.UVW01, D);
					Accumulate(Interpolation.UVX1, Interpolation.UVY1, Interpolation.UVW11, D);
				}
			}

			// F_ut (input 4..5)
			for (int32 Channel = 0; Channel < 2; ++Channel)
			{
				const double D = InputGrad[4 + Channel];
				auto Accumulate = [this, Channel](int32 X, int32 T, double Weight, double Grad)
				{
					UTGroup.Gradients[(T * Config.UTSize + X) * 2 + Channel] += Weight * Grad;
				};
				Accumulate(Interpolation.UTX0, Interpolation.UTT0, Interpolation.UTW00, D);
				Accumulate(Interpolation.UTX1, Interpolation.UTT0, Interpolation.UTW10, D);
				Accumulate(Interpolation.UTX0, Interpolation.UTT1, Interpolation.UTW01, D);
				Accumulate(Interpolation.UTX1, Interpolation.UTT1, Interpolation.UTW11, D);
			}

			// F_vt (input 6..7)
			for (int32 Channel = 0; Channel < 2; ++Channel)
			{
				const double D = InputGrad[6 + Channel];
				auto Accumulate = [this, Channel](int32 X, int32 T, double Weight, double Grad)
				{
					VTGroup.Gradients[(T * Config.VTSize + X) * 2 + Channel] += Weight * Grad;
				};
				Accumulate(Interpolation.VTX0, Interpolation.VTT0, Interpolation.VTW00, D);
				Accumulate(Interpolation.VTX1, Interpolation.VTT0, Interpolation.VTW10, D);
				Accumulate(Interpolation.VTX0, Interpolation.VTT1, Interpolation.VTW01, D);
				Accumulate(Interpolation.VTX1, Interpolation.VTT1, Interpolation.VTW11, D);
			}

			// F_uvt (input 8..11)
			const int32 T0 = Interpolation.UVTT0;
			const int32 T1 = Interpolation.UVTT1;
			const double TemporalWeight0 = Interpolation.UVTW001; // see SampleUVT: temporal weights stored in ...001/...111
			const double TemporalWeight1 = Interpolation.UVTW111;
			for (int32 Channel = 0; Channel < 4; ++Channel)
			{
				const double D = InputGrad[8 + Channel];

				auto Accumulate = [this, Channel, T0, T1, TemporalWeight0, TemporalWeight1](int32 X, int32 Y, double SpatialWeight, double Grad)
				{
					const double GradT0 = SpatialWeight * TemporalWeight0 * Grad;
					const double GradT1 = SpatialWeight * TemporalWeight1 * Grad;
					if (UsesBCSampling())
					{
						UVTMaps[T0].ScatterTexelGradient(X, Y, Channel, GradT0);
						UVTMaps[T1].ScatterTexelGradient(X, Y, Channel, GradT1);
					}
					else
					{
						UVTRawGroup.Gradients[((T0 * Config.UVTSize + Y) * Config.UVTSize + X) * 4 + Channel] += GradT0;
						UVTRawGroup.Gradients[((T1 * Config.UVTSize + Y) * Config.UVTSize + X) * 4 + Channel] += GradT1;
					}
				};

				Accumulate(Interpolation.UVTX0, Interpolation.UVTY0, Interpolation.UVTW000, D);
				Accumulate(Interpolation.UVTX1, Interpolation.UVTY0, Interpolation.UVTW100, D);
				Accumulate(Interpolation.UVTX0, Interpolation.UVTY1, Interpolation.UVTW010, D);
				Accumulate(Interpolation.UVTX1, Interpolation.UVTY1, Interpolation.UVTW110, D);
			}
		}

		void FTileReference::ZeroAllGradients()
		{
			MLPGroup.ZeroGradients();
			UTGroup.ZeroGradients();
			VTGroup.ZeroGradients();
			if (Config.bSimulateBC)
			{
				UVE1Group.ZeroGradients();
				UVE2Group.ZeroGradients();
				UVWGroup.ZeroGradients();
				UVTE1Group.ZeroGradients();
				UVTE2Group.ZeroGradients();
				UVTWGroup.ZeroGradients();
				UVMap.ZeroGradients();
				for (FBCMap& Map : UVTMaps)
				{
					Map.ZeroGradients();
				}
			}
			else
			{
				UVRawGroup.ZeroGradients();
				UVTRawGroup.ZeroGradients();
			}
		}

		void FTileReference::ConvertBCGradients()
		{
			if (!Config.bSimulateBC)
			{
				return;
			}

			UVMap.Backward();
			FMemory::Memcpy(UVE1Group.Gradients.GetData(), UVMap.DE1.GetData(), UVMap.DE1.Num() * sizeof(double));
			FMemory::Memcpy(UVE2Group.Gradients.GetData(), UVMap.DE2.GetData(), UVMap.DE2.Num() * sizeof(double));
			FMemory::Memcpy(UVWGroup.Gradients.GetData(), UVMap.DW.GetData(), UVMap.DW.Num() * sizeof(double));

			int32 SliceBlocks = UVTMaps.Num() > 0 ? UVTMaps[0].NumBlocks() : 0;
			for (int32 Slice = 0; Slice < UVTMaps.Num(); ++Slice)
			{
				FBCMap& Map = UVTMaps[Slice];
				Map.Backward();
				FMemory::Memcpy(&UVTE1Group.Gradients[Slice * SliceBlocks * 4], Map.DE1.GetData(), Map.DE1.Num() * sizeof(double));
				FMemory::Memcpy(&UVTE2Group.Gradients[Slice * SliceBlocks * 4], Map.DE2.GetData(), Map.DE2.Num() * sizeof(double));
				FMemory::Memcpy(&UVTWGroup.Gradients[Slice * SliceBlocks * 16], Map.DW.GetData(), Map.DW.Num() * sizeof(double));
			}
		}

		void FTileReference::EvaluatePoints(const TArray<FVector>& Coords, TArray<float>& OutRGB) const
		{
			OutRGB.SetNumUninitialized(Coords.Num() * OutputDim);
			for (int32 Index = 0; Index < Coords.Num(); ++Index)
			{
				double Input[InputDim];
				FSampleInterpolation Interpolation;
				Sample(Coords[Index].X, Coords[Index].Y, Coords[Index].Z, nullptr, Input, Interpolation);

				FMLPForwardCache Cache;
				Forward(MLP, Input, Cache);
				for (int32 Channel = 0; Channel < OutputDim; ++Channel)
				{
					OutRGB[Index * OutputDim + Channel] = static_cast<float>(Cache.Output[Channel]);
				}
			}
		}

		void FTileReference::ReconstructFeatureTexels(TArray<double>& OutUV, TArray<double>& OutUVT) const
		{
			check(UsesBCSampling());

			OutUV.SetNumUninitialized(Config.UVSize * Config.UVSize * 4);
			for (int32 Y = 0; Y < Config.UVSize; ++Y)
			{
				for (int32 X = 0; X < Config.UVSize; ++X)
				{
					for (int32 Channel = 0; Channel < 4; ++Channel)
					{
						OutUV[(Y * Config.UVSize + X) * 4 + Channel] = UVMap.GetTexel(X, Y, Channel);
					}
				}
			}

			const int32 Slices = UVTMaps.Num();
			OutUVT.SetNumUninitialized(Slices * Config.UVTSize * Config.UVTSize * 4);
			for (int32 Slice = 0; Slice < Slices; ++Slice)
			{
				const FBCMap& Map = UVTMaps[Slice];
				for (int32 Y = 0; Y < Config.UVTSize; ++Y)
				{
					for (int32 X = 0; X < Config.UVTSize; ++X)
					{
						for (int32 Channel = 0; Channel < 4; ++Channel)
						{
							const int64 Index = (static_cast<int64>(Slice) * Config.UVTSize * Config.UVTSize + Y * Config.UVTSize + X) * 4 + Channel;
							OutUVT[Index] = Map.GetTexel(X, Y, Channel);
						}
					}
				}
			}
		}

		void FTileReference::SyncBCGroupsToMaps()
		{
			if (!Config.bSimulateBC)
			{
				return;
			}

			FMemory::Memcpy(UVMap.E1.GetData(), UVE1Group.Values.GetData(), UVMap.E1.Num() * sizeof(double));
			FMemory::Memcpy(UVMap.E2.GetData(), UVE2Group.Values.GetData(), UVMap.E2.Num() * sizeof(double));
			FMemory::Memcpy(UVMap.W.GetData(), UVWGroup.Values.GetData(), UVMap.W.Num() * sizeof(double));

			const int32 SliceBlocks = UVTMaps.Num() > 0 ? UVTMaps[0].NumBlocks() : 0;
			for (int32 Slice = 0; Slice < UVTMaps.Num(); ++Slice)
			{
				FBCMap& Map = UVTMaps[Slice];
				FMemory::Memcpy(Map.E1.GetData(), &UVTE1Group.Values[Slice * SliceBlocks * 4], Map.E1.Num() * sizeof(double));
				FMemory::Memcpy(Map.E2.GetData(), &UVTE2Group.Values[Slice * SliceBlocks * 4], Map.E2.Num() * sizeof(double));
				FMemory::Memcpy(Map.W.GetData(), &UVTWGroup.Values[Slice * SliceBlocks * 16], Map.W.Num() * sizeof(double));
			}
		}

		void FTileReference::UpdateParameters()
		{
			MLPGroup.Update();
			UTGroup.Update();
			VTGroup.Update();
			UTGroup.Clamp01();
			VTGroup.Clamp01();

			if (Config.bSimulateBC)
			{
				UVE1Group.Update();
				UVE2Group.Update();
				UVWGroup.Update();
				UVTE1Group.Update();
				UVTE2Group.Update();
				UVTWGroup.Update();
				UVE1Group.Clamp01();
				UVE2Group.Clamp01();
				UVWGroup.Clamp01();
				UVTE1Group.Clamp01();
				UVTE2Group.Clamp01();
				UVTWGroup.Clamp01();

				SyncBCGroupsToMaps();
			}
			else
			{
				UVRawGroup.Update();
				UVTRawGroup.Update();
				UVRawGroup.Clamp01();
				UVTRawGroup.Clamp01();
			}

			UnflattenParameters(MLPGroup.Values, MLP);
		}

		void FTileReference::TrainStep(TConstArrayView<FTrainSample> Batch, TConstArrayView<float> Noise, double& OutMeanLoss)
		{
			check(Noise.Num() == 0 || Noise.Num() == Batch.Num() * 12);

			ZeroAllGradients();

			FMLPWeights MLPGradients;
			MLP.ZeroGradients(MLPGradients);
			TArray<double> FlatGradients;

			double TotalLoss = 0.0;
			for (int32 SampleIndex = 0; SampleIndex < Batch.Num(); ++SampleIndex)
			{
				const FTrainSample& TrainSample = Batch[SampleIndex];
				double Input[InputDim];
				FSampleInterpolation Interpolation;

				double NoiseLocal[12];
				const double* NoisePtr = nullptr;
				if (Noise.Num() > 0)
				{
					for (int32 Component = 0; Component < 12; ++Component)
					{
						NoiseLocal[Component] = static_cast<double>(Noise[SampleIndex * 12 + Component]);
					}
					NoisePtr = NoiseLocal;
				}

				Sample(TrainSample.U, TrainSample.V, TrainSample.Time, NoisePtr, Input, Interpolation);

				FMLPForwardCache Cache;
				Forward(MLP, Input, Cache);
				TotalLoss += Loss(Cache, TrainSample.Target);

				double InputGrad[InputDim];
				Backward(MLP, Input, Cache, TrainSample.Target, MLPGradients, InputGrad);
				ScatterInputGradient(Interpolation, InputGrad);
			}

			FlattenParameters(MLPGradients, MLPGroup.Gradients);
			ConvertBCGradients();
			UpdateParameters();

			OutMeanLoss = Batch.Num() > 0 ? TotalLoss / Batch.Num() : 0.0;
		}

		void FTileReference::TrainStep(TConstArrayView<FTrainSample> Batch, FRandomStream& NoiseStream, double& OutMeanLoss)
		{
			TArray<float> Noise;
			Noise.SetNumUninitialized(Batch.Num() * 12);
			const float Alpha = static_cast<float>(Config.NoiseAlpha);
			for (float& Value : Noise)
			{
				Value = (NoiseStream.FRand() - 0.5f) * Alpha;
			}
			TrainStep(Batch, Noise, OutMeanLoss);
		}

		void FTileReference::ExportFlatParameters(TArray<double>& Out) const
		{
			check(Config.bSimulateBC);
			Out.Reset();
			Out.Append(MLPGroup.Values);
			Out.Append(UVE1Group.Values);
			Out.Append(UVE2Group.Values);
			Out.Append(UVWGroup.Values);
			Out.Append(UVTE1Group.Values);
			Out.Append(UVTE2Group.Values);
			Out.Append(UVTWGroup.Values);
			Out.Append(UTGroup.Values);
			Out.Append(VTGroup.Values);
		}

		void FTileReference::ImportFlatParameters(const TArray<double>& In)
		{
			check(Config.bSimulateBC);
			const int32 Expected = MLPGroup.Values.Num() + UVE1Group.Values.Num() + UVE2Group.Values.Num() + UVWGroup.Values.Num()
				+ UVTE1Group.Values.Num() + UVTE2Group.Values.Num() + UVTWGroup.Values.Num()
				+ UTGroup.Values.Num() + VTGroup.Values.Num();
			check(In.Num() == Expected);

			int32 Offset = 0;
			auto CopyInto = [&In, &Offset](TArray<double>& Target)
			{
				FMemory::Memcpy(Target.GetData(), &In[Offset], Target.Num() * sizeof(double));
				Offset += Target.Num();
			};
			CopyInto(MLPGroup.Values);
			CopyInto(UVE1Group.Values);
			CopyInto(UVE2Group.Values);
			CopyInto(UVWGroup.Values);
			CopyInto(UVTE1Group.Values);
			CopyInto(UVTE2Group.Values);
			CopyInto(UVTWGroup.Values);
			CopyInto(UTGroup.Values);
			CopyInto(VTGroup.Values);

			SyncBCGroupsToMaps();
			UnflattenParameters(MLPGroup.Values, MLP);
		}

		void FTileReference::FinalizeCompression()
		{
			if (!Config.bSimulateBC)
			{
				// Post-hoc BC encoding of the freely trained maps (mirrors applying BC after training).
				auto EncodeMap = [](FBCMap& Map, const TArray<double>& Raw, int32 Channels)
				{
					for (int32 Block = 0; Block < Map.NumBlocks(); ++Block)
					{
						const int32 BlockX = (Block % Map.BlocksX) * 4;
						const int32 BlockY = (Block / Map.BlocksX) * 4;
						for (int32 Channel = 0; Channel < Channels; ++Channel)
						{
							double MinValue = MAX_dbl;
							double MaxValue = -MAX_dbl;
							for (int32 P = 0; P < 16; ++P)
							{
								const int32 X = BlockX + (P % 4);
								const int32 Y = BlockY + (P / 4);
								const double Value = FMath::Clamp(Raw[(Y * Map.SizeX + X) * Channels + Channel], 0.0, 1.0);
								MinValue = FMath::Min(MinValue, Value);
								MaxValue = FMath::Max(MaxValue, Value);
							}
							Map.E1[Block * Channels + Channel] = MinValue;
							Map.E2[Block * Channels + Channel] = MaxValue;
							const double Range = MaxValue - MinValue;
							for (int32 P = 0; P < 16; ++P)
							{
								const int32 X = BlockX + (P % 4);
								const int32 Y = BlockY + (P / 4);
								const double Value = FMath::Clamp(Raw[(Y * Map.SizeX + X) * Channels + Channel], 0.0, 1.0);
								Map.W[Block * 16 + P] = (Range > WeightEpsilon)
									? FMath::Clamp((Value - MinValue) / Range, 0.0, 1.0)
									: 0.0;
							}
						}
					}
				};

				EncodeMap(UVMap, UVRawGroup.Values, 4);
				for (int32 Slice = 0; Slice < UVTMaps.Num(); ++Slice)
				{
					TArray<double> SliceRaw;
					SliceRaw.SetNumUninitialized(Config.UVTSize * Config.UVTSize * 4);
					FMemory::Memcpy(SliceRaw.GetData(),
						&UVTRawGroup.Values[Slice * Config.UVTSize * Config.UVTSize * 4],
						SliceRaw.Num() * sizeof(double));
					EncodeMap(UVTMaps[Slice], SliceRaw, 4);
				}

				bFinalized = true;
			}

			// 8-bit quantization of the final representation (both variants).
			if (UsesBCSampling())
			{
				auto QuantizeMap = [](FBCMap& Map)
				{
					Quantize8Bit(Map.E1);
					Quantize8Bit(Map.E2);
					Quantize8Bit(Map.W);
				};
				QuantizeMap(UVMap);
				for (FBCMap& Map : UVTMaps)
				{
					QuantizeMap(Map);
				}
			}
			Quantize8Bit(UTGroup.Values);
			Quantize8Bit(VTGroup.Values);
		}

		void FTileReference::Evaluate(const TArray<FTrainSample>& Samples, double& OutMSE, double& OutPSNR) const
		{
			if (Samples.Num() == 0)
			{
				OutMSE = 0.0;
				OutPSNR = 0.0;
				return;
			}

			double SumSquared = 0.0;
			double MaxTarget = 0.0;
			for (const FTrainSample& SamplePoint : Samples)
			{
				double Input[InputDim];
				FSampleInterpolation Interpolation;
				Sample(SamplePoint.U, SamplePoint.V, SamplePoint.Time, nullptr, Input, Interpolation);

				FMLPForwardCache Cache;
				Forward(MLP, Input, Cache);
				for (int32 Channel = 0; Channel < OutputDim; ++Channel)
				{
					const double Diff = Cache.Output[Channel] - SamplePoint.Target[Channel];
					SumSquared += Diff * Diff;
					MaxTarget = FMath::Max(MaxTarget, FMath::Abs(SamplePoint.Target[Channel]));
				}
			}

			OutMSE = SumSquared / static_cast<double>(Samples.Num() * OutputDim);
			const double Range = FMath::Max(MaxTarget, 1e-6);
			OutPSNR = (OutMSE > 0.0) ? 10.0 * FMath::LogX(10.0, Range * Range / OutMSE) : 99.0;
		}

		void FTileReference::EvaluatePerOutput(const TArray<FTrainSample>& Samples, double OutPSNR[OutputDim]) const
		{
			for (int32 Channel = 0; Channel < OutputDim; ++Channel)
			{
				OutPSNR[Channel] = 0.0;
			}
			if (Samples.Num() == 0)
			{
				return;
			}

			double Squared[OutputDim] = {};
			double MaxTarget[OutputDim] = {};
			for (const FTrainSample& SamplePoint : Samples)
			{
				double Input[InputDim];
				FSampleInterpolation Interpolation;
				Sample(SamplePoint.U, SamplePoint.V, SamplePoint.Time, nullptr, Input, Interpolation);

				FMLPForwardCache Cache;
				Forward(MLP, Input, Cache);
				for (int32 Channel = 0; Channel < OutputDim; ++Channel)
				{
					const double Diff = Cache.Output[Channel] - SamplePoint.Target[Channel];
					Squared[Channel] += Diff * Diff;
					MaxTarget[Channel] = FMath::Max(MaxTarget[Channel], FMath::Abs(SamplePoint.Target[Channel]));
				}
			}

			for (int32 Channel = 0; Channel < OutputDim; ++Channel)
			{
				const double Mse = Squared[Channel] / static_cast<double>(Samples.Num());
				const double Range = FMath::Max(MaxTarget[Channel], 1e-6);
				OutPSNR[Channel] = Mse > 0.0 ? 10.0 * FMath::LogX(10.0, Range * Range / Mse) : 99.0;
			}
		}

		// ---------------------------------------------------------------------------
		// Probe loss + finite-difference check
		// ---------------------------------------------------------------------------

		void FTileReference::SetProbeWeights(const double ProbeWeights_[12])
		{
			FMemory::Memcpy(ProbeWeights, ProbeWeights_, sizeof(ProbeWeights));
		}

		double FTileReference::ComputeProbeLoss(double U, double V, double Time) const
		{
			double Input[InputDim];
			FSampleInterpolation Interpolation;
			Sample(U, V, Time, nullptr, Input, Interpolation);

			double Total = 0.0;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				Total += ProbeWeights[Index] * Input[Index];
				Total += ProbeWeights[8 + Index] * Input[8 + Index];
			}
			Total += ProbeWeights[4] * Input[4] + ProbeWeights[5] * Input[5];
			Total += ProbeWeights[6] * Input[6] + ProbeWeights[7] * Input[7];
			return Total;
		}

		FGradientCheckResult FTileReference::CheckScatterGradients(int32 Seed, int32 NumSamples)
		{
			FGradientCheckResult Result;

			FRandomStream Stream(Seed);

			TArray<double> SampleU;
			TArray<double> SampleV;
			TArray<double> SampleT;
			SampleU.SetNumUninitialized(NumSamples);
			SampleV.SetNumUninitialized(NumSamples);
			SampleT.SetNumUninitialized(NumSamples);
			for (int32 Index = 0; Index < NumSamples; ++Index)
			{
				SampleU[Index] = static_cast<double>(Stream.FRand());
				SampleV[Index] = static_cast<double>(Stream.FRand());
				SampleT[Index] = static_cast<double>(Stream.FRand());
			}

			double Probe[12];
			for (double& Value : Probe)
			{
				Value = 2.0 * static_cast<double>(Stream.FRand()) - 1.0;
			}
			SetProbeWeights(Probe);

			auto ComputeBatchProbeLoss = [this, &SampleU, &SampleV, &SampleT]() -> double
			{
				double Total = 0.0;
				for (int32 Index = 0; Index < SampleU.Num(); ++Index)
				{
					Total += ComputeProbeLoss(SampleU[Index], SampleV[Index], SampleT[Index]);
				}
				return Total;
			};

			// Analytic pass: scatter the probe gradient through the production scatter path.
			ZeroAllGradients();
			for (int32 Index = 0; Index < NumSamples; ++Index)
			{
				double Input[InputDim];
				FSampleInterpolation Interpolation;
				Sample(SampleU[Index], SampleV[Index], SampleT[Index], nullptr, Input, Interpolation);

				double ProbeInputGrad[InputDim] = {};
				for (int32 Component = 0; Component < 4; ++Component)
				{
					ProbeInputGrad[Component] = Probe[Component];
					ProbeInputGrad[8 + Component] = Probe[8 + Component];
				}
				ProbeInputGrad[4] = Probe[4];
				ProbeInputGrad[5] = Probe[5];
				ProbeInputGrad[6] = Probe[6];
				ProbeInputGrad[7] = Probe[7];

				ScatterInputGradient(Interpolation, ProbeInputGrad);
			}
			ConvertBCGradients();

			// Gather the active parameter groups.
			TArray<FParameterGroup*> Groups;
			Groups.Add(&UTGroup);
			Groups.Add(&VTGroup);
			if (Config.bSimulateBC)
			{
				Groups.Add(&UVE1Group);
				Groups.Add(&UVE2Group);
				Groups.Add(&UVWGroup);
				Groups.Add(&UVTE1Group);
				Groups.Add(&UVTE2Group);
				Groups.Add(&UVTWGroup);
			}
			else
			{
				Groups.Add(&UVRawGroup);
				Groups.Add(&UVTRawGroup);
			}

			// Finite differences over every parameter.
			const double Epsilon = 1e-6;
			const double AbsTolerance = 1e-8;
			const double RelTolerance = 1e-5;
			double MaxViolation = -MAX_dbl;

			for (FParameterGroup* Group : Groups)
			{
				for (int32 Index = 0; Index < Group->Values.Num(); ++Index)
				{
					const double Original = Group->Values[Index];

					Group->Values[Index] = Original + Epsilon;
					SyncBCGroupsToMaps();
					const double LossPlus = ComputeBatchProbeLoss();

					Group->Values[Index] = Original - Epsilon;
					SyncBCGroupsToMaps();
					const double LossMinus = ComputeBatchProbeLoss();

					Group->Values[Index] = Original;
					SyncBCGroupsToMaps();

					const double Numeric = (LossPlus - LossMinus) / (2.0 * Epsilon);
					const double Analytic = Group->Gradients[Index];
					const double AbsError = FMath::Abs(Numeric - Analytic);
					const double Scale = FMath::Max(FMath::Abs(Numeric), FMath::Abs(Analytic));
					const double RelError = AbsError / FMath::Max(Scale, 1e-12);

					MaxViolation = FMath::Max(MaxViolation, AbsError - (AbsTolerance + RelTolerance * Scale));
					if (RelError > Result.MaxRelError)
					{
						Result.MaxRelError = RelError;
						Result.WorstNumeric = Numeric;
						Result.WorstAnalytic = Analytic;
					}
					Result.MaxAbsError = FMath::Max(Result.MaxAbsError, AbsError);
					++Result.NumCheckedParameters;
				}
			}

			Result.bPassed = MaxViolation < 0.0;
			return Result;
		}
	}
}
