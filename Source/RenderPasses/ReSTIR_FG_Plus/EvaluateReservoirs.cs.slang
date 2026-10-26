#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.RaytracingInline;
import Utils.Sampling.SampleGenerator;
import Utils.Color.ColorHelpers;
import Rendering.RTXDI.RTXDI;
import Rendering.PhotonGuiding.PhotonGuiding;
import StructsAndHelpers;

Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gView;

StructuredBuffer<PathReservoir> gPathReservoir;
StructuredBuffer<CausticReservoir> gCausticReservoir;
Texture2D<uint2> gCausticReservoirGuidingData;

RWTexture2D<PackedHitInfo> gVBufferPrev;
RWTexture2D<float4> gViewPrev;
RWTexture2D<float4> gOutColor;

cbuffer CB
{
    uint gFrameCount;         // Current it for sample gen
    uint2 gFrameDim;          // Frame Dimensions for camera dir
    float _pad;
}

static const uint kRNGNumPasses = RNG_NUM_PASSES;
static const bool kUseEnvBackground = USE_ENV_BACKROUND;
static const uint kMIHints = (uint)MaterialInstanceHints::AdjustShadingNormal;
static const bool kDisableDirectLight = DISABLE_DIRECT;
static const bool kDisableIndirectLight = DISABLE_INDIRECT;
static const bool kDisableCaustics = DISABLE_CAUSTICS;
static const bool kUsePhotonGuiding = USE_PHOTON_GUIDING;

float3 evaluateCausticReservoir(uint2 pixel, inout SampleGenerator sg) {
    float3 outColor = float3(0);
    CausticReservoir causticReservoir = gCausticReservoir[index2Dto1D(pixel, gFrameDim.x)];
    if (!causticReservoir.isValid())
        return outColor;

    // Get surface and shading data
    HitInfo hitInfo = HitInfo(causticReservoir.sampleSurface);
    let lod = ExplicitLodTextureSampler(0.f);
    ShadingData sd = loadShadingData(hitInfo, causticReservoir.sampleSurfaceView, lod);
    let mi = gScene.materials.getMaterialInstance(sd, lod, kMIHints);
    let bsdfProperties = mi.getProperties(sd);

    //Backprojected Caustic
    float3 directionToCamera = normalize(gScene.camera.getPosition() - sd.posW);
    outColor = causticReservoir.wSum * causticReservoir.photonRadiance * mi.eval(sd, directionToCamera, sg);

    if (kUsePhotonGuiding)
    {
        PhotonGuidingData pgd = PhotonGuidingData(gCausticReservoirGuidingData[pixel]);
        gPhotonGuiding.addGuidingContribution(pgd.lightIndex, pgd.guidingPixels, luminance(outColor));
    }

    return outColor;
}

[numthreads(16, 16, 1)]
void main(uint2 pixel: SV_DispatchThreadID)
{
    if(any(pixel >= gFrameDim))
        return;

    HitInfo hitInfo = HitInfo(gVBuffer[pixel]);
    float3 viewDir = gView[pixel].xyz;

    float3 outColor = float3(0);
    if (hitInfo.isValid()) {
        SampleGenerator sg = SampleGenerator(pixel, gFrameCount * kRNGNumPasses + (kRNGNumPasses - 1));
        let lod = ExplicitLodTextureSampler(0.f);

        // Get Material properties
        ShadingData sd = loadShadingData(hitInfo, viewDir, lod);
        let mi = gScene.materials.getMaterialInstance(sd, lod, kMIHints);
        let bsdfProperties = mi.getProperties(sd);

        // Add Emissive
        float3 directLight = bsdfProperties.emission;

        //Eval ReSTIR DI
        float dist = 1.f;
        float3 dir = {};
        float3 Li = {};
        bool rtxdiValid = gRTXDI.getFinalSample(pixel, dir, dist, Li);
        
        if (rtxdiValid)
        {
            rtxdiValid = dispatchShadowRay(sd, dir, dist);
        }
        if (rtxdiValid)
        {
            directLight += mi.eval(sd, dir, sg) * Li;
        }

        if (!kDisableDirectLight)
            outColor += directLight;

        //Evaluate directly visible caustics
        float3 caustic = evaluateCausticReservoir(pixel, sg);
        if (!kDisableCaustics)
            outColor += caustic;

        // Evaluate Path Reservoir
        PathReservoir pathReservoir = gPathReservoir[index2Dto1D(pixel, gFrameDim.x)];
        float3 indirectLight = pathReservoir.F * pathReservoir.weight;
        if (kUsePhotonGuiding)
        {
            if (pathReservoir.flags.isRadianceEstimate || pathReservoir.flags.isFinalGather)
            {
                PhotonGuidingData pgd = PhotonGuidingData(pathReservoir.packedGuidingData);
                gPhotonGuiding.addGuidingContribution(pgd.lightIndex, pgd.guidingPixels, luminance(indirectLight));
            }
        }
        if (!kDisableIndirectLight)
            outColor += indirectLight;

    }else
    {
        outColor = gScene.envMap.eval(viewDir);
    }

    gVBufferPrev[pixel] = hitInfo.getData();
    gViewPrev[pixel] = float4(viewDir, 1);
    gOutColor[pixel] = float4(outColor, 1.0);
}
