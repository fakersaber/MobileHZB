#pragma once
#include "CoreMinimal.h"
#include "RenderGraph.h"
#include "RHIUtilities.h"

class FViewInfo;
class FRDGBuilder; 

#define USE_LOW_RESLUTION 1

//ViewState to HzbSystem
struct FMobileHzbSystem {
	FMobileHzbSystem();
	~FMobileHzbSystem();
	
	//Register Function
	static void MobileBuildHzb(FRHICommandListImmediate& RHICmdList, const FViewInfo& View);
	static void RegisterViewStateToSystem(const uint32 SceneViewStateUniqueID);
	static void UnRegisterViewStateToSystem(const uint32 SceneViewStateUniqueID);

	static FMobileHzbSystem* GetHzbSystemByViewStateUniqueId(const FSceneViewState* ViewStatePtr);
	void InitGPUResources();
	void ReduceMips(FRDGTextureSRVRef RDGTexutreMip, FRDGTextureRef RDGFurthestHZBTexture, const FViewInfo& View, FRDGBuilder& GraphBuilder, uint32 CurOutHzbMipLevel);
	void MobileRasterBuildHZB(FRHICommandListImmediate& RHICmdList, const FViewInfo& View);
	void MobileComputeBuildHZB(FRHICommandListImmediate& RHICmdList, const FViewInfo& View);
	const FRWBufferStructured& GetStructuredBufferRes() { return MobileHZBBuffer_GPU; }
	
	FIntPoint HzbSize;
	FRWBufferStructured MobileHZBBuffer_GPU;
	TRefCountPtr<IPooledRenderTarget> MobileHZBTexture;

	static TMap<uint32, TUniquePtr<FMobileHzbSystem>> ViewUniqueId2HzbSystemMap;

#if USE_LOW_RESLUTION
	static constexpr int32 GroupSizeX = 8;
	static constexpr int32 GroupSizeY = 8;
	static constexpr int32 kHzbTexWidth = 256;
	static constexpr int32 kHzbTexHeight = 128;
	static constexpr uint8 kHZBMaxMipmap = 8;
#else
	static constexpr int32 GroupSizeX = 16;
	static constexpr int32 GroupSizeY = 16;
	static constexpr int32 kHzbTexWidth = 512;
	static constexpr int32 kHzbTexHeight = 256;
	static constexpr uint8 kHZBMaxMipmap = 9;
#endif
	static constexpr bool bUseRaster = false;
};
