#include "RuntimeRender.h"

#include "RuntimeDrawTrianglePass.h"
#include "SceneRendering.h"

/*-----------------------------------------------------------------------------
	RuntimeRender
-----------------------------------------------------------------------------*/
RuntimeRender::RuntimeRender(const FSceneViewFamily* InViewFamily, FRuntimeSceneView* RuntimeSceneView, FHitProxyConsumer* HitProxyConsumer)
	: FSceneRenderer(InViewFamily, HitProxyConsumer),
	RuntimeSceneView(*RuntimeSceneView)
{
	// pre load
	LoadObject<UTexture2D>(nullptr, TEXT("/Engine/Textures/T_UE_Logo_M.T_UE_Logo_M"));
}


void RuntimeRender::Render(FRDGBuilder& GraphBuilder, const FSceneRenderUpdateInputs* SceneUpdateInputs)
{
	FRDGTextureRef ViewFamilyTexture = TryCreateRuntimeSceneViewTexture(GraphBuilder, RuntimeSceneView);
	
	AddRuntimeDrawTrianglePass(GraphBuilder, ViewFamilyTexture);
}
