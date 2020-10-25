#pragma once
#include "CoreMinimal.h"
#include "RenderGraph.h"

class FViewInfo;
class FRDGBuilder; 

class FMobileHZB {
public:
	static void InitialResource(FRHICommandListImmediate& RHICmdList, FViewInfo& View);
	static void ReduceMips(FRDGTextureSRVRef RDGTexutreMip, FRDGTextureRef RDGFurthestHZBTexture, FViewInfo& View, FRDGBuilder& GraphBuilder, uint32 DstMipLevel);
	static void MobileBuildHZB(FRHICommandListImmediate& RHICmdList, FViewInfo& View);

	static constexpr bool bUseCompute = true;
	static constexpr int32 kMaxMipBatchSize = 1;
};
