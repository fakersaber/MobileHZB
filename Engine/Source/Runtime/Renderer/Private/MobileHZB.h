#pragma once
#include "CoreMinimal.h"
#include "RenderGraph.h"

class FViewInfo;
class FRDGBuilder; 

struct FMobileHzbResource{
	FMobileHzbResource(FRHICommandListImmediate& RHICmdList, FViewInfo& View);
	~FMobileHzbResource();

	FRWBuffer MobileHzbBuffer;
	TRefCountPtr<IPooledRenderTarget> MobileHZBTexture;
	FIntPoint HzbSize;
};


struct FMobileHzbSystem {

	friend class FHZBOcclusionTester;
	friend class FMobileSceneRenderer;

	static constexpr bool bUseCompute = false;
	static constexpr uint8 kHZBTestMaxMipmap = 8;
	static constexpr int32 kMaxMipBatchSize = 1;

	static void InitialResource(FRHICommandListImmediate& RHICmdList, FViewInfo& View);
	static void ReleaseResource();

	static void ReduceMips(FRDGTextureSRVRef RDGTexutreMip, FRDGTextureRef RDGFurthestHZBTexture, FViewInfo& View, FRDGBuilder& GraphBuilder, uint32 CurOutHzbMipLevel);
	static void MobileRasterBuildHZB(FRHICommandListImmediate& RHICmdList, FViewInfo& View);

	static void ReduceBuffer(FRDGTextureSRVRef RDGTexutreMip, const FRWBuffer& MobileHzbBuffer, FViewInfo& View, FRDGBuilder& GraphBuilder, uint32 CurOutHzbMipLevel);
	static void MobileComputeBuildHZB(FRHICommandListImmediate& RHICmdList, FViewInfo& View);

private:
	static TUniquePtr<FMobileHzbResource> MobileHzbResourcesPtr;
};
