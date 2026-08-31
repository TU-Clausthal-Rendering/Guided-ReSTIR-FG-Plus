#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.RaytracingInline;
import Utils.Sampling.SampleGenerator;
import Utils.Color.ColorHelpers;
import Utils.Math.MathHelpers;
import Rendering.RTXDI.RTXDI;
import StructsAndHelpers;

cbuffer CB
{
    uint gFrameCount; // Current it for sample gen
    uint2 gFrameDim;  // Frame Dimensions
    uint gConfidenceCap;  //Confidence Cap

    float4x4 gPrevCamViewProjection; //Previous Camera view projection for splatting

    float3 gPrevCamPos; // Previous camera position for splatting
    uint _pad;

    float3 gPrevCamForward; // Previous camera forward for splatting
    uint _pad2;
}

Texture2D<float2> gMVec;

StructuredBuffer<CausticReservoir> gCausticReservoirPrev;
StructuredBuffer<int> gLightTraceLinkedList; // next pointer

RWStructuredBuffer<CausticReservoir> gCausticReservoir;
RWTexture2D<int> gLightTraceHeadCounter; // Head index for the linked list

// Photon Guiding (can be nullptr)
RWTexture2D<uint2> gCausticReservoirGuidingData;
Texture2D<uint2> gCausticReservoirGuidingDataPrev;

static const uint kRNGNumPasses = RNG_NUM_PASSES;
static const uint kMIHints = (uint)MaterialInstanceHints::AdjustShadingNormal;
static const bool kUsePhotonGuiding = USE_PHOTON_GUIDING;

float getSplattingMISWeight(float3 toCam, float distToCam, float3 camForward, CausticReservoir r, ShadingData sd, IMaterialInstance mi, inout SampleGenerator sg, out float jacobianWeight, out float targetFunction)
{
    jacobianWeight = getBackprojectJacobian(gFrameDim.y, toCam, distToCam);
    jacobianWeight *= dot(toCam, sd.getOrientedFaceNormal());
    float jacobianShifted = jacobianWeight / r.subPixelJacobian;
    const float3 bsdf = mi.eval(sd, toCam, sg);
    targetFunction = luminance(r.photonRadiance * bsdf);
    float misP = targetFunction * jacobianShifted;
    misP = isnan(misP) ? 0.0f : misP;
    return misP;
}

[numthreads(16, 16, 1)]
void main(uint2 pixel: SV_DispatchThreadID)
{
    //Sample Gen and LOD sampler
    let lod = ExplicitLodTextureSampler(0.0);
    SampleGenerator sg = SampleGenerator(pixel, gFrameCount * kRNGNumPasses + 4);

    CausticReservoir rCurr = gCausticReservoir[index2Dto1D(pixel, gFrameDim.x)];
    PhotonGuidingData pgd = PhotonGuidingData();
    if (kUsePhotonGuiding)
        pgd = PhotonGuidingData(gCausticReservoirGuidingData[pixel]);

    float wSum = 0;
    // Shift current to last frame using the previous camera
    if (rCurr.isValid())
    {
        //Get photon surface
        HitInfo hit = HitInfo(rCurr.sampleSurface);
        ShadingData sd = loadShadingData(hit, rCurr.sampleSurfaceView, lod);

        //Connect to previous camera
        float3 toPrevCam = gPrevCamPos - sd.posW;
        float distToPrevCam = length(toPrevCam);
        toPrevCam /= distToPrevCam;

        // Check if sample was in projection matrix last frame and if it is visible
        float3 prevScreenPos = getCameraProjection(gPrevCamViewProjection, sd.posW);
        bool isVisible = (all(prevScreenPos.xyz >= 0) && all(prevScreenPos.xyz < 1));
        if (isVisible)
        {
            float visRayDist = distToPrevCam - gScene.camera.data.nearZ; // Remove camera near
            isVisible = dispatchShadowRay(sd, toPrevCam, visRayDist, false);
        }

        // Calc contribution
        float p1 = 0;
        uint c1 = 0;
        if (isVisible)
        {
            let mi = gScene.materials.getMaterialInstance(sd, lod, kMIHints);
            // Calc jacobian and contribution
            float jacobianWeight, targetFunction;
            p1 = getSplattingMISWeight(toPrevCam, distToPrevCam, gPrevCamForward, rCurr, sd, mi, sg, jacobianWeight, targetFunction);
            // Get Confidence Weight from the reservoir at the backprojected pixel
            uint2 prevPixel = uint2(floor(prevScreenPos.xy * gFrameDim));
            CausticReservoir rPrev = gCausticReservoirPrev[index2Dto1D(prevPixel, gFrameDim.x)];
            c1 = rPrev.c;
        }
        float misWeight = evalMIS(rCurr.targetFunction, rCurr.c, p1, c1);
        wSum = misWeight * rCurr.targetFunction * rCurr.wSum;
    }

    // Shift the other samples using a linked list
    const float3 currentCamPosW = gScene.camera.getPosition();
    const float3 currentCamForward = normalize(gScene.camera.data.cameraW);
    const uint currentConfidence = rCurr.c;
    int headIndex = gLightTraceHeadCounter[pixel];
    while (headIndex >= 0)
    {
        CausticReservoir rPrev = gCausticReservoirPrev[headIndex];
        if (rPrev.isValid()) //Should always be true, else it would not be in the linked list
        {
            // Get photon surface
            HitInfo hit = HitInfo(rPrev.sampleSurface);
            ShadingData sd = loadShadingData(hit, rPrev.sampleSurfaceView, lod);

            // MIS weight for current (Visibility was already tested in a previous pass)
            float3 toCurrentCam = currentCamPosW - sd.posW;
            float distToCurrentCam = length(toCurrentCam);
            toCurrentCam /= distToCurrentCam;

            //Get Contribution
            let mi = gScene.materials.getMaterialInstance(sd, lod, kMIHints);
            float subPixelJacobian, targetFunction;
            float p1 = getSplattingMISWeight(toCurrentCam, distToCurrentCam, currentCamForward, rPrev, sd, mi, sg, subPixelJacobian, targetFunction);

            //Calc Reservoir weigth
            float jacobian = subPixelJacobian / rPrev.subPixelJacobian;
            float misWeight = evalMIS(rPrev.targetFunction, rPrev.c, p1, currentConfidence); 
            float w = misWeight * targetFunction * rPrev.wSum * jacobian;
            wSum += w;

            // Check if the temporal sample replaces the current sample in the reservoir
            if ((sampleNext1D(sg) * wSum) < w)
            {
                rCurr = rPrev;
                rCurr.targetFunction = targetFunction;
                rCurr.subPixelJacobian = subPixelJacobian;
                if (kUsePhotonGuiding)
                    pgd = PhotonGuidingData(gCausticReservoirGuidingDataPrev[index1Dto2D(headIndex, gFrameDim.x)]);
            }
        }
        //Update head index for next iteration
        int newHeadIndex = gLightTraceLinkedList[headIndex];
        headIndex = newHeadIndex;
    }

    // Use motion vector and bilinear interpolation for new confidence weight
    float newConfidence = float(currentConfidence);
    float2 prevPixel = float2(pixel) + gMVec[pixel] * gFrameDim;
    int2 prevPixelTopLeft = int2(floor(prevPixel));
    float2 fractionalCoord = saturate(prevPixel - float2(prevPixelTopLeft));
    [unroll]
    for (uint x = 0; x < 2; x++) {
        [unroll]
        for (uint y = 0; y < 2; y++)
        {
            int2 prevPixel = prevPixelTopLeft + int2(x, y);
            if (any(prevPixel < 0) || any(prevPixel >= gFrameDim))
                continue;

            CausticReservoir rPrev = gCausticReservoirPrev[index2Dto1D(prevPixel, gFrameDim.x)];
            float bilinearWeight = lerp(1.0 - x, x, fractionalCoord.x) * lerp(1.0 - y, y, fractionalCoord.y);
            newConfidence += bilinearWeight * rPrev.c;
        }
    }

    rCurr.c = clamp(uint(round(newConfidence)), 1, gConfidenceCap);
    // Finalize reservoir (wSum -> W)
    if (rCurr.targetFunction > 0)
        rCurr.wSum = wSum / rCurr.targetFunction;
    else
        rCurr.wSum = 0;

    //Store final reservoir
    gCausticReservoir[index2Dto1D(pixel, gFrameDim.x)] = rCurr;
    // Reset head buffer for linked list
    gLightTraceHeadCounter[pixel] = -1;
    if (kUsePhotonGuiding)
        gCausticReservoirGuidingData[pixel] = pgd.pack();
}
