
#include "RuntimeDrawTrianglePass.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

// 顶点着色器
class FSimpleVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSimpleVS);
	SHADER_USE_PARAMETER_STRUCT(FSimpleVS, FGlobalShader)
	
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FMatrix44f, MVP)
		SHADER_PARAMETER(FMatrix44f, Model)
		SHADER_PARAMETER(FMatrix44f, View)
		SHADER_PARAMETER(FMatrix44f, Projection)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

// 像素着色器
class FSimplePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSimplePS);
	SHADER_USE_PARAMETER_STRUCT(FSimplePS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputTextureSampler)
		SHADER_PARAMETER(FVector3f, LightPosision)
		SHADER_PARAMETER(FVector3f, ViewPosision)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FSimpleVS, "/Engine/Private/RuntimeDrawTriangleShader/RuntimeDrawShader.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FSimplePS, "/Engine/Private/RuntimeDrawTriangleShader/RuntimeDrawShader.usf", "MainPS", SF_Pixel);

struct FVetexData
{
	FVector3f position;
	FVector2f uv;
};

void AddRuntimeDrawTrianglePass(FRDGBuilder& GraphBuilder, FRDGTextureRef ViewFamilyTexture)
{
	// todo: 准备顶点数据
	TArray<FVetexData> Vertices = {
		{FVector3f(-0.5f,  0.5f, 0.0f), FVector2f(0.0f, 0.0f)}, // 0 左上
		{FVector3f(0.5f,  0.5f, 0.0f), FVector2f(1.0f, 0.0f)}, // 1 右上
		{FVector3f(0.5f, -0.5f, 0.0f), FVector2f(1.0f, 1.0f)}, // 2 左下
		{FVector3f( -0.5f, -0.5f, 0.0f), FVector2f(0.0f,1.0f)} // 3 右下
	};

	// todo: 准备indexbuffer
	TArray<uint16> Indices = {
		0, 1, 2,  // 第一个三角形：左上、左下、右上
		0, 2, 3   // 第二个三角形：右上、左下、右下
	};

	const uint32 DataSize = Vertices.Num() * sizeof(FVetexData);

	// 创建RDG顶点缓冲区
	FRDGBufferRef VertexBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVetexData), Vertices.Num()), TEXT("VertexBuffer"));
	GraphBuilder.QueueBufferUpload(VertexBuffer, Vertices.GetData(), DataSize, ERDGInitialDataFlags::None);

	//生成indexbuffer
	FRDGBufferRef IndexBuffer = GraphBuilder.CreateBuffer(
	FRDGBufferDesc::CreateBufferDesc(sizeof(uint16), Indices.Num()), TEXT("indexBuffer"));
	GraphBuilder.QueueBufferUpload(IndexBuffer, Indices.GetData(), Indices.Num() * sizeof(uint16), ERDGInitialDataFlags::None);

	// 该纹理在RuntimeRender创建时(game thread)预加载, 否则在render thread初次加载会崩溃
	UTexture2D* tex = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/Textures/T_UE_Logo_M.T_UE_Logo_M"));
	TRefCountPtr<IPooledRenderTarget> PooledRenderTarget = CreateRenderTarget(tex->GetResource()->GetTextureRHI(), TEXT("RenderTarget"));
	FRDGTextureRef texture = GraphBuilder.RegisterExternalTexture(PooledRenderTarget, TEXT("mytexture"));
	
	// 渲染目标绑定游戏视口（viewport）的后缓冲区（backBuffer）
	auto *PassParameters = GraphBuilder.AllocParameters<FSimpleVS::FParameters>();
	PassParameters->RenderTargets[0] = FRenderTargetBinding(ViewFamilyTexture, ERenderTargetLoadAction::EClear);

	// 设置采样器
	FSamplerStateInitializerRHI SamplerDesc;
	SamplerDesc.Filter = SF_AnisotropicLinear;  // 各向异性线性过滤
	SamplerDesc.AddressU = AM_Wrap;             // U方向重复
	SamplerDesc.AddressV = AM_Clamp;            // V方向钳制
	SamplerDesc.AddressW = AM_Wrap;
	SamplerDesc.MipBias = 0.0f;
	SamplerDesc.MinMipLevel = 0;
	SamplerDesc.MaxMipLevel = 15;
	SamplerDesc.MaxAnisotropy = 8;             // 各向异性级别
	FRHISamplerState* SamplerState = RHICreateSamplerState(SamplerDesc);
	
	// 设置给ps准备的uniform变量
	auto *PsPassParameters = GraphBuilder.AllocParameters<FSimplePS::FParameters>();
	PsPassParameters->InputTexture = texture;
	PsPassParameters->InputTextureSampler = SamplerState;
	
	// 添加RDG绘制Pass
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("RuntimeDrawTrianglePass"), 
		PassParameters,
		ERDGPassFlags::Raster, 
		[VertexBuffer, texture, IndexBuffer, PsPassParameters](FRHICommandList &RHICmdList)
		{
			// 创建顶点声明（仅包含位置）
			FVertexDeclarationElementList Elements;
			Elements.Add(FVertexElement(0, 0, VET_Float3, 0, sizeof(FVetexData)));
			Elements.Add(FVertexElement(0, sizeof(FVector3f), VET_Float2, 1, sizeof(FVetexData)));
			
			// 设置图形管线状态
			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
			GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

			// 绑定Shader
			TShaderMapRef<FSimpleVS> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			TShaderMapRef<FSimplePS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			FVertexDeclarationRHIRef VertexDecl = RHICreateVertexDeclaration(Elements);
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = VertexDecl;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;

			// 提交PSO
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 1);

			// 设置uniform变量
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PsPassParameters); // 注意要在提交PSO之后执行

			// 绑定顶点流
			RHICmdList.SetStreamSource(0, VertexBuffer->GetRHI(), 0);

			// 提交绘制命令
			RHICmdList.DrawIndexedPrimitive(
			IndexBuffer->GetRHI(), 
			/*BaseVertexIndex=*/ 0,
			/*MinIndex=*/ 0,
			/*NumVertices=*/ 4,
			/*StartIndex=*/ 0,
			/*NumPrimitives=*/ 2,
			/*NumInstances=*/ 1
			);
		});
	
}