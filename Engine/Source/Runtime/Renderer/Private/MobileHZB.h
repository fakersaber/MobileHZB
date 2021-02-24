#pragma once
#include "CoreMinimal.h"
#include "RenderGraph.h"
#include "RHIUtilities.h"

class FViewInfo;
class FRDGBuilder; 

struct FMobileHzbResource : public FRenderResource {
	FMobileHzbResource() { }
	~FMobileHzbResource() { };

	virtual void InitDynamicRHI() override; 
	virtual void ReleaseDynamicRHI() override;
	
	FRWBufferStructured MobileHZBBuffer_GPU;
	TRefCountPtr<IPooledRenderTarget> MobileHZBTexture;
	FIntPoint HzbSize;
};


struct FMobileHzbSystem {

	friend class FHZBOcclusionTester;
	friend class FMobileSceneRenderer;

	static constexpr int32 GroupSizeX = 8;
	static constexpr int32 GroupSizeY = 8;
	static constexpr int32 kHzbTexWidth = 256;
	static constexpr int32 kHzbTexHeight = 128;
	static constexpr uint8 kHZBMaxMipmap = 8;
	static constexpr bool bUseRaster = false;

	static void InitialResource();

	static void ReduceMips(FRDGTextureSRVRef RDGTexutreMip, FRDGTextureRef RDGFurthestHZBTexture, FViewInfo& View, FRDGBuilder& GraphBuilder, uint32 CurOutHzbMipLevel);
	static void MobileRasterBuildHZB(FRHICommandListImmediate& RHICmdList, FViewInfo& View);

	static void MobileComputeBuildHZB(FRHICommandListImmediate& RHICmdList, FViewInfo& View);

	static FRWBufferStructured* GetStructuredBufferRes() {
		return &MobileHzbResourcesPtr->MobileHZBBuffer_GPU;
	}


private:
	static TGlobalResource<FMobileHzbResource>* MobileHzbResourcesPtr;
};
