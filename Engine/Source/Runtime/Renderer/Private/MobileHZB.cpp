#include "MobileHZB.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "PixelShaderUtils.h"

#define SL_USE_MOBILEHZB 1

TAutoConsoleVariable<int32> CVarMobileUseRaster(
	TEXT("r.GpuDriven.MobileUseRaster"),
	FMobileHzbSystem::bUseRaster,
	TEXT("Test Mali Device Raster"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<int32> CVarMobileBuildHZB(
	TEXT("r.GpuDriven.MobileBuildHZB"),
	1,
	TEXT("Test Mali Device BuildHZB"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<int32> CVarMobileUseSceneDepth(
	TEXT("r.GpuDriven.MobileHZBUseSceneDepth"),
	1,
	TEXT("Test UseSceneDepth"),
	ECVF_RenderThreadSafe
);


DECLARE_CYCLE_STAT(TEXT("HZBOcclusion Generator"), STAT_CLMM_HZBOcclusionGenerator, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("HZBOcclusion Submit"), STAT_CLMM_HZBCopyOcclusionSubmit, STATGROUP_CommandListMarkers);

TMap<uint32, TUniquePtr<FMobileHzbSystem>> FMobileHzbSystem::ViewUniqueId2HzbSystemMap;

BEGIN_SHADER_PARAMETER_STRUCT(FMobileHZBParameters, )
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ParentTextureMip)
	SHADER_PARAMETER_SAMPLER(SamplerState, ParentTextureMipSampler)
END_SHADER_PARAMETER_STRUCT()


class FMobileHZBBuildPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMobileHZBBuildPS);
	SHADER_USE_PARAMETER_STRUCT(FMobileHZBBuildPS, FGlobalShader)

	class FDimSceneDepth : SHADER_PERMUTATION_BOOL("FDimSceneDepth"); //第一个Pass,从alpha中取LinearDepth
	class FUseSceneDepth : SHADER_PERMUTATION_BOOL("UseSceneDepth"); //测试使用深度图是否剔除更多

	using FPermutationDomain = TShaderPermutationDomain<FDimSceneDepth, FUseSceneDepth>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FMobileHZBParameters, Shared)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

};

IMPLEMENT_GLOBAL_SHADER(FMobileHZBBuildPS, "/Engine/Private/MobileHZB.usf", "HZBBuildPS", SF_Pixel);
	
void FMobileHzbSystem::ReduceMips(FRDGTextureSRVRef RDGTexutreMip, FRDGTextureRef RDGFurthestHZBTexture, const FViewInfo& View, FRDGBuilder& GraphBuilder, uint32 CurOutHzbMipLevel) {

	//Mip为0可能有两种情况,所以需要额外DstMipLevel

	FIntPoint SrcSize = FIntPoint::DivideAndRoundUp(RDGTexutreMip->Desc.Texture->Desc.Extent, 1 << RDGTexutreMip->Desc.MipLevel);
	FIntPoint DstSize = FIntPoint::DivideAndRoundUp(HzbSize, 1 << CurOutHzbMipLevel);

	FMobileHZBParameters ShaderParameters;

	ShaderParameters.ParentTextureMip = RDGTexutreMip;
	ShaderParameters.ParentTextureMipSampler = TStaticSamplerState<SF_Point>::GetRHI();


	FMobileHZBBuildPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FMobileHZBBuildPS::FParameters>();
	PassParameters->Shared = ShaderParameters;
	PassParameters->RenderTargets[0] = FRenderTargetBinding(RDGFurthestHZBTexture, ERenderTargetLoadAction::ENoAction, CurOutHzbMipLevel);

	FMobileHZBBuildPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FMobileHZBBuildPS::FDimSceneDepth>(CurOutHzbMipLevel == 0);  //use SceneTexture Only Mipmap is 0 
	PermutationVector.Set<FMobileHZBBuildPS::FUseSceneDepth>(CVarMobileUseSceneDepth.GetValueOnAnyThread() != 0);
	

	TShaderMapRef<FMobileHZBBuildPS> PixelShader(View.ShaderMap, PermutationVector);

	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		View.ShaderMap,
		RDG_EVENT_NAME("DownsampleHZB(mip=%d) %dx%d", CurOutHzbMipLevel, DstSize.X, DstSize.Y),
		PixelShader,
		PassParameters,
		FIntRect(0, 0, DstSize.X, DstSize.Y)
	);
}

void FMobileHzbSystem::MobileRasterBuildHZB(FRHICommandListImmediate& RHICmdList, const FViewInfo& View) {

	const FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	FRDGBuilder GraphBuilder(RHICmdList);
	const auto& SceneTexture = SceneContext.GetSceneColor();

	FRDGTextureRef RDGSceneTexutre = GraphBuilder.RegisterExternalTexture(SceneTexture, TEXT("RDGSceneTexture"));
	FRDGTextureSRVRef RDGSceneTexutreMip = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(RDGSceneTexutre, 0));

	//RG的FRDGTextureSRVDesc也是封装FRHITextureSRVCreateInfo,也可以直接手动管理
	FRDGTextureRef RDGFurthestHZBTexture = GraphBuilder.RegisterExternalTexture(MobileHZBTexture);
	ReduceMips(RDGSceneTexutreMip, RDGFurthestHZBTexture, View, GraphBuilder, 0);

	// Reduce the next mips
	int32 MaxMipBatchSize = 1;
	for (int32 StartDestMip = MaxMipBatchSize; StartDestMip < RDGFurthestHZBTexture->Desc.NumMips; StartDestMip += MaxMipBatchSize) {
		FRDGTextureSRVRef RDGHzbSrvMip = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(RDGFurthestHZBTexture, StartDestMip - 1));
		ReduceMips(RDGHzbSrvMip, RDGFurthestHZBTexture, View, GraphBuilder, StartDestMip);
	}

	// Update the view. this only used for SceneOcclusion, no need 
	//View.HZBMipmap0Size = HzbSize;
	GraphBuilder.Execute();
}

class FMobileHZBBuildCSLevel0 : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMobileHZBBuildCSLevel0);

public:

	FMobileHZBBuildCSLevel0() : FGlobalShader() {}

	FMobileHZBBuildCSLevel0(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer) {

		ParentSceneTexture.Bind(Initializer.ParameterMap, TEXT("ParentSceneTexture"));
		ParentSceneTextureSampler.Bind(Initializer.ParameterMap, TEXT("ParentSceneTextureSampler"));
		HzbStructuredBufferUAV.Bind(Initializer.ParameterMap, TEXT("HzbStructuredBufferUAV_Zero"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) {
		return true;
	}

	void BindParameters(FRHICommandList& RHICmdList, const FViewInfo& View, const FTextureRHIRef& SceneTexture, const FRWBufferStructured& HzbStructuredBuffer) {
		RHICmdList.Transition(FRHITransitionInfo(SceneTexture, ERHIAccess::DSVWrite, ERHIAccess::SRVCompute)); //RAW
		// There are cases where we ping-pong images between UAVCompute and SRVCompute. In that case it may be more efficient to leave the image in VK_IMAGE_LAYOUT_GENERAL
		// (at the very least, it will mean fewer image barriers). There's no good way to detect this though, so it might be better if the high level code just did UAV
		// to UAV transitions in that case, instead of SRV <-> UAV.
		RHICmdList.Transition(FRHITransitionInfo(HzbStructuredBuffer.UAV, ERHIAccess::SRVCompute, ERHIAccess::UAVCompute)); //WAR
		SetTextureParameter(RHICmdList, RHICmdList.GetBoundComputeShader(), ParentSceneTexture, SceneTexture);
		SetSamplerParameter(RHICmdList, RHICmdList.GetBoundComputeShader(), ParentSceneTextureSampler, TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());
		SetUAVParameter(RHICmdList, RHICmdList.GetBoundComputeShader(), HzbStructuredBufferUAV, HzbStructuredBuffer.UAV);
	}


	void UnBindParameters(FRHICommandList& RHICmdList) {
		SetUAVParameter(RHICmdList, RHICmdList.GetBoundComputeShader(), HzbStructuredBufferUAV, nullptr);
	}

private:

	LAYOUT_FIELD(FShaderResourceParameter, ParentSceneTexture);
	LAYOUT_FIELD(FShaderResourceParameter, ParentSceneTextureSampler);
	LAYOUT_FIELD(FShaderResourceParameter, HzbStructuredBufferUAV);
};

class FMobileHZBBuildCSLevel1 : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMobileHZBBuildCSLevel1);

public:

	FMobileHZBBuildCSLevel1() : FGlobalShader() {}

	FMobileHZBBuildCSLevel1(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer) {
		HzbStructuredBufferUAV.Bind(Initializer.ParameterMap, TEXT("HzbStructuredBufferUAV_One"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) {
		return true;
	}

	void BindParameters(FRHICommandList& RHICmdList, const FViewInfo& View, const FRWBufferStructured& HzbStructuredBuffer) {
		RHICmdList.Transition(FRHITransitionInfo(HzbStructuredBuffer.UAV, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute)); //WAW
		SetUAVParameter(RHICmdList, RHICmdList.GetBoundComputeShader(), HzbStructuredBufferUAV, HzbStructuredBuffer.UAV);
	}

	void UnBindParameters(FRHICommandList& RHICmdList) {
		SetUAVParameter(RHICmdList, RHICmdList.GetBoundComputeShader(), HzbStructuredBufferUAV, nullptr);
	}

private:
	LAYOUT_FIELD(FShaderResourceParameter, HzbStructuredBufferUAV);
};

IMPLEMENT_GLOBAL_SHADER(FMobileHZBBuildCSLevel0, "/Engine/Private/MobileHZB.usf", "HZBBuildCSLevelZero", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMobileHZBBuildCSLevel1, "/Engine/Private/MobileHZB.usf", "HZBBuildCSLevelOne", SF_Compute);

void FMobileHzbSystem::MobileComputeBuildHZB(FRHICommandListImmediate& RHICmdList, const FViewInfo& View) {

	//Level0
	{
		const FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

		if (CVarMobileUseSceneDepth.GetValueOnAnyThread() != 0) {
			const auto& SceneTexture = SceneContext.SceneDepthZ->GetRenderTargetItem().ShaderResourceTexture;
			constexpr auto DispatchX = FMobileHzbSystem::kHzbTexWidth / GroupSizeX;
			constexpr auto DispatchY = FMobileHzbSystem::kHzbTexHeight / GroupSizeY;
			TShaderMapRef<FMobileHZBBuildCSLevel0> HzbGeneratorShader(View.ShaderMap);
			RHICmdList.SetComputeShader(HzbGeneratorShader.GetComputeShader());
			HzbGeneratorShader->BindParameters(RHICmdList, View, SceneTexture, MobileHZBBuffer_GPU);
			RHICmdList.DispatchComputeShader(DispatchX, DispatchY, 1);
			HzbGeneratorShader->UnBindParameters(RHICmdList);
		}
		else {
			const auto& SceneTexture = SceneContext.GetSceneColorTexture();
			constexpr auto DispatchX = FMobileHzbSystem::kHzbTexWidth / GroupSizeX;
			constexpr auto DispatchY = FMobileHzbSystem::kHzbTexHeight / GroupSizeY;
			TShaderMapRef<FMobileHZBBuildCSLevel0> HzbGeneratorShader(View.ShaderMap);
			RHICmdList.SetComputeShader(HzbGeneratorShader.GetComputeShader());
			HzbGeneratorShader->BindParameters(RHICmdList, View, SceneTexture, FMobileHzbSystem::GetStructuredBufferRes());
			RHICmdList.DispatchComputeShader(DispatchX, DispatchY, 1);
			HzbGeneratorShader->UnBindParameters(RHICmdList);
		}
	}

	//Level1
	{
		constexpr auto DispatchX = 2;
		constexpr auto DispatchY = 1;
		TShaderMapRef<FMobileHZBBuildCSLevel1> HzbGeneratorShader(View.ShaderMap);
		RHICmdList.SetComputeShader(HzbGeneratorShader.GetComputeShader());
		HzbGeneratorShader->BindParameters(RHICmdList, View, FMobileHzbSystem::GetStructuredBufferRes());
		RHICmdList.DispatchComputeShader(DispatchX, DispatchY, 1);
		HzbGeneratorShader->UnBindParameters(RHICmdList);
	}
}

FMobileHzbSystem::FMobileHzbSystem() 
	: HzbSize(FIntPoint::ZeroValue)
{
	//不想每次创建就马上创建RHI资源,单独放在Render函数中
}

FMobileHzbSystem::~FMobileHzbSystem() {
	MobileHZBBuffer_GPU.Release();
	MobileHZBTexture.SafeRelease(); //#TODO: GlobleRender Resources释放时机晚于SceneRenderTarget, 不能使用RT POOL管理, 直接释放
}

void FMobileHzbSystem::RegisterViewStateToSystem(const uint32 SceneViewStateUniqueID) {
	check(!ViewUniqueId2HzbSystemMap.Contains(SceneViewStateUniqueID));
	ViewUniqueId2HzbSystemMap.Emplace(SceneViewStateUniqueID, MakeUnique<FMobileHzbSystem>());
}

void FMobileHzbSystem::UnRegisterViewStateToSystem(const uint32 SceneViewStateUniqueID) {
	const uint32 NumRemove = ViewUniqueId2HzbSystemMap.Remove(SceneViewStateUniqueID);
	check(NumRemove == 1);
}

FMobileHzbSystem* FMobileHzbSystem::GetHzbSystemByViewStateUniqueId(const FSceneViewState* ViewStatePtr) {
	if (ViewStatePtr) {
		const TUniquePtr<FMobileHzbSystem>& FoundSystem = ViewUniqueId2HzbSystemMap.FindChecked(ViewStatePtr->UniqueID);
		check(FoundSystem.Get());
		return FoundSystem.Get();
	}
	return nullptr;
}

void FMobileHzbSystem::InitGPUResources() {

	if(FMobileHzbSystem::bUseRaster && HzbSize == FIntPoint::ZeroValue) {
		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();

		HzbSize = FIntPoint(FMobileHzbSystem::kHzbTexWidth, FMobileHzbSystem::kHzbTexHeight);
		int32 NumMips = FMobileHzbSystem::kHZBMaxMipmap;
		//CreateResource
		FPooledRenderTargetDesc MobileHZBFurthestDesc = FPooledRenderTargetDesc::Create2DDesc(
			HzbSize,
			PF_R16F,
			FClearValueBinding::None, TexCreate_HideInVisualizeTexture, TexCreate_ShaderResource | TexCreate_RenderTargetable, false, NumMips);
#if SL_USE_MOBILEHZB
		GRenderTargetPool.FindFreeElement(RHICmdList, MobileHZBFurthestDesc, MobileHZBTexture, TEXT("MobileHZBFurthest"), ERenderTargetTransience::NonTransient);
#else
		GRenderTargetPool.FindFreeElement(RHICmdList, MobileHZBFurthestDesc, View.HZB, TEXT("MobileHZBFurthest"), ERenderTargetTransience::NonTransient);
#endif
		return;
	}


	if(!FMobileHzbSystem::bUseRaster && MobileHZBBuffer_GPU.NumBytes == 0){
		constexpr int32 PerElementSize = sizeof(float);
		constexpr int32 BufferElements = FMobileHzbSystem::kHzbTexWidth * FMobileHzbSystem::kHzbTexHeight * 2;
		MobileHZBBuffer_GPU.Initialize(PerElementSize, BufferElements, BUF_Static);
		return;
	}
}

void FMobileHzbSystem::MobileBuildHzb(FRHICommandListImmediate& RHICmdList, const FViewInfo& View) {
	//Hiz generator
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (CVarMobileBuildHZB.GetValueOnAnyThread() == 0) {
		return;
	}
	RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_HZBOcclusionGenerator));
#endif

	FMobileHzbSystem* FoundSystem = FMobileHzbSystem::GetHzbSystemByViewStateUniqueId(View.ViewState);
	if(FoundSystem) {
		if (
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			CVarMobileUseRaster.GetValueOnAnyThread() != 0
#else
			FMobileHzbSystem::bUseRaster
#endif
			)
		{
			FoundSystem->MobileRasterBuildHZB(RHICmdList, View);
		}
		else {
			FoundSystem->MobileComputeBuildHZB(RHICmdList, View);
		}
	}
}

