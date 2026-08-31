#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.RaytracingInline;
import Utils.Sampling.SampleGenerator;
import Utils.Color.ColorHelpers;
import Utils.Math.MathHelpers;
import StructsAndHelpers;

cbuffer CB
{
    uint gSGSeed;                       // Current iteration for sample gen
    uint gReservoirOffsetSGSeed;        // SG seed for reservoir offset
    uint2 gFrameDim;                    // Frame Dimensions

    uint gConfidenceCap;                //Confidence Cap
    float gSpatialRadius;               // Radius for spatial resampling
    float gNormalThreshold;             // Normal angle threshold
    float gJacobianDistanceThreshold;   // Jacobian distance threshold

    float gRelativeDepthThreshold;      // Relative depth treshold
    uint gNumResamplingPass;            // Resampling iteration. == 0 -> Temporal, else Spatial
}

//Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<PackedHitInfo> gVBufferPrev;
Texture2D<float4> gView;
Texture2D<float4> gViewPrev;
Texture2D<float2> gMVec;

StructuredBuffer<Reservoir> gPathReservoirOther;

Texture2D<float4> gShiftData;
Texture2D<float4> gShiftDataOther;
StructuredBuffer<ShiftedSurface> gShiftSurface;
StructuredBuffer<ShiftedSurface> gShiftSurfaceOther;
Texture2D<float4> gShiftRcThp;
Texture2D<float4> gShiftRcThpOther;

//Outputs
RWStructuredBuffer<Reservoir> gPathReservoir;

static const uint kMIHints = (uint)MaterialInstanceHints::AdjustShadingNormal;
static const bool kShiftReconnectionSample = SHIFT_RECONNECTION_SAMPLE;

bool resampleReservoir(inout Reservoir reservoir, Reservoir rCurr, Reservoir rOther, int2 pixel, ShadingData sdCurr, ShadingData sdOther, inout SampleGenerator sg) {
    // For spatial resampling, check if surfaces are similar
    const bool isSpatialResampling = gNumResamplingPass > 0;
    const bool loadShiftThp = !isSpatialResampling && kShiftReconnectionSample;
    if (isSpatialResampling) {
        const float3 cameraPos = gScene.camera.getPosition();
        float currentDepth = distance(cameraPos, sdCurr.posW);
        float temporalDepth = distance(cameraPos, sdOther.posW);
        bool normalThreshold = abs(dot(sdCurr.getOrientedFaceNormal(), sdOther.getOrientedFaceNormal())) < gNormalThreshold;
        bool depthThreshold = abs(currentDepth - temporalDepth) > gRelativeDepthThreshold * currentDepth;

        if (normalThreshold || depthThreshold)
            return false;
    }

    // Init needed reservoir variables
    let lod = ExplicitLodTextureSampler(0.f);
    float w_0 = 0;
    float w_1 = 0;
    float wSum = 0;

    // Get w for current reservoir. This involves calculating the correct MIS weight with the shifted reservoir sample.
    bool reservoirContributes = rCurr.isValid();
    if (reservoirContributes)
    {
        //Calc MIS weight
        float mis = 1;
        bool isReconnection = rCurr.flags.isReconnectionSurface;
        float4 shiftData = float4(0);
        bool loadShiftData = !isReconnection || (isReconnection && rCurr.rcPathLength > 1);
        if (loadShiftData)
            shiftData = gShiftData[pixel]; // xyz = F, w = jacobian

        // For not reconnection or reconnections with random replay, shift is already calculated
        if (!isReconnection) {
            if (shiftData.w > 0) {
                float3 shiftedF = shiftData.xyz * (shiftData.w / rCurr.jacobian);
                mis = evalMIS(rCurr.c, luminance(rCurr.F), rOther.c, luminance(shiftedF));
            }
        }
        else //Reconnection
        {
            //Load surface before reconnection for rcPathLenght > 1 (was shifted)
            bool surfaceValid = true;
            float3 thp = 1;
            if (rCurr.rcPathLength > 1)
            {
                ShiftedSurface shiftSurface = gShiftSurface[index2Dto1D(pixel, gFrameDim.x)];
                HitInfo shiftHit = HitInfo(shiftSurface.packedHit);
                surfaceValid = shiftHit.isValid();
                if (surfaceValid)
                {
                    sdOther = loadShadingData(shiftHit, -shiftSurface.Wi, lod);
                    thp = shiftData.xyz;
                }
            }

            // Get reconnection surface
            HitInfo sampleHit = HitInfo(rCurr.reconnectionSurface);
            if (surfaceValid && sampleHit.isValid())
            {
                let mi = gScene.materials.getMaterialInstance(sdOther, lod, kMIHints);
                ShadingData sdSample = loadShadingDataFromOrigin(sampleHit, sdOther.posW, lod);

                // Calculate new jacobian
                float3 toSample = sdSample.posW - sdOther.posW;
                float distToSample = length(toSample);
                toSample /= distToSample;
                // Set lobes that were originally used to sample
                uint activeLobes = rCurr.flags.reconLastHitSpecular ? (uint)LobeType::Specular | (uint)LobeType::Delta : (uint)LobeType::Diffuse;
                sdOther.mtl.setActiveLobes(activeLobes);

                float pdf = mi.evalPdf(sdOther, toSample);
                float jacobianNew = abs(dot(sdSample.getOrientedFaceNormal(), -toSample)) / (distToSample * distToSample);
                jacobianNew *= pdf;

                float jacobian = jacobianNew / rCurr.jacobian;

                // Jacobian out
                if (!rCurr.flags.isNeeAtRecon)
                {
                    let miSample = gScene.materials.getMaterialInstance(sdSample, lod, kMIHints);
                    activeLobes = rCurr.flags.reconSpecular ? (uint)LobeType::Specular | (uint)LobeType::Delta : (uint)LobeType::Diffuse;
                    sdSample.mtl.setActiveLobes(activeLobes);
                    jacobian *= miSample.evalPdf(sdSample, rCurr.sampleWo) / rCurr.jacobianOutgoing;
                }

                float3 F = float3(0);
                // Load shifted rcThp if the reconnection sample was also shifted
                if (loadShiftThp)
                {
                    rCurr.rcThp = gShiftRcThp[pixel].xyz;
                }

                // BSDF and Visibility test
                if (jacobian > 0 && (distToSample > gJacobianDistanceThreshold) && any(rCurr.rcThp > 0)) {
                    if (dispatchShadowRay(sdOther, toSample, distToSample)) {
                        float3 bsdf = mi.eval(sdOther, toSample, sg) / pdf;
                        F += bsdf * jacobian * rCurr.rcThp * thp;
                    }
                }

                // Calc MIS weight
                mis = evalMIS(rCurr.c, luminance(rCurr.F), rOther.c, luminance(F));
            }
        }

        //New w_0
        w_0 = mis * luminance(rCurr.F) * rCurr.weight;
        wSum += w_0;
    }

    if (w_0 > 0)
        reservoir = rCurr;

    //Calculate w for the resampled reservoir. Requires shifting the reservoir to current sample space
    float3 Fother = float3(0);
    float jacobianOther = 1;
    float jacobianOtherOut = 1;
    bool otherCanBeSelected = rOther.isValid();
    otherCanBeSelected &= !isnan(rOther.weight) && !isinf(rOther.weight);
    if (otherCanBeSelected)
    {
        float jacobian = 0.0;
        bool isReconnection = rOther.flags.isReconnectionSurface;
        float4 shiftData = float4(0);
        bool loadShiftData = !isReconnection || (isReconnection && rOther.rcPathLength > 1);
        if (loadShiftData)
            shiftData = gShiftDataOther[pixel]; // xyz = F, w = jacobian
        // When only random replay was used
        if (!isReconnection) {
            if (shiftData.w > 0)
            {
                jacobianOther = shiftData.w;
                jacobian = jacobianOther / rOther.jacobian;
                Fother = shiftData.xyz;
            }
        }
        else // Reconnection
        {
            // Load surface before reconnection for rcPathLenght > 1 (was shifted)
            bool surfaceValid = true;
            float3 thp = 1;
            if (rOther.rcPathLength > 1)
            {
                ShiftedSurface shiftSurface = gShiftSurfaceOther[index2Dto1D(pixel, gFrameDim.x)];
                HitInfo shiftHit = HitInfo(shiftSurface.packedHit);
                surfaceValid = shiftHit.isValid();

                if (surfaceValid)
                {
                    sdCurr = loadShadingData(shiftHit, -shiftSurface.Wi, lod);
                    thp = shiftData.xyz;
                } else
                    thp = 0;
            }

            // Get reconnection surface
            HitInfo sampleHit = HitInfo(rOther.reconnectionSurface);
            if (sampleHit.isValid() && surfaceValid)
            {
                let mi = gScene.materials.getMaterialInstance(sdCurr, lod, kMIHints);
                ShadingData sdSample = loadShadingDataFromOrigin(sampleHit, sdCurr.posW, lod);

                // Calculate new jacobian
                float3 toSample = sdSample.posW - sdCurr.posW;
                float distToSample = length(toSample);
                toSample /= distToSample;
                // Set lobes that were originally used to sample
                uint activeLobes = rOther.flags.reconLastHitSpecular ? (uint)LobeType::Specular | (uint)LobeType::Delta : (uint)LobeType::Diffuse;
                sdCurr.mtl.setActiveLobes(activeLobes);

                float pdf = mi.evalPdf(sdCurr, toSample);
                jacobianOther = abs(dot(sdSample.getOrientedFaceNormal(), -toSample)) / (distToSample * distToSample);
                jacobianOther *= pdf;
                jacobian = jacobianOther / rOther.jacobian;

                // Jacobian out
                if (!rOther.flags.isNeeAtRecon)
                {
                    let miSample = gScene.materials.getMaterialInstance(sdSample, lod, kMIHints);
                    activeLobes = rOther.flags.reconSpecular ? (uint)LobeType::Specular | (uint)LobeType::Delta : (uint)LobeType::Diffuse;
                    sdSample.mtl.setActiveLobes(activeLobes);
                    jacobianOtherOut = miSample.evalPdf(sdSample, rOther.sampleWo);
                    jacobian *= jacobianOtherOut / rOther.jacobianOutgoing;
                }

                //Load shifted rcThp if the reconnection sample was also shifted
                if (loadShiftThp)
                {
                    rOther.rcThp = gShiftRcThpOther[pixel].xyz;
                }

                // BSDF and Visibility test
                if (jacobian > 0 && (distToSample > gJacobianDistanceThreshold) && any(rOther.rcThp > 0))
                {
                    if (dispatchShadowRay(sdCurr, toSample, distToSample))
                    {
                        float3 bsdf = mi.eval(sdCurr, toSample, sg) / pdf;
                        Fother = rOther.rcThp * bsdf * thp;
                    }
                }
            }          
        }
        float estFprev = luminance(Fother) * jacobian;
        float mis = evalMIS(rOther.c, luminance(rOther.F), rCurr.c, estFprev);
        w_1 = mis * estFprev * rOther.weight;

        wSum += w_1;
    }

    uint cSum = min(rCurr.c + rOther.c, gConfidenceCap);

    //Randomly select new reservoir depending on relative weight
    bool selectOtherSample = (sampleNext1D(sg) * wSum) < w_1 && otherCanBeSelected;
    if (selectOtherSample) {
        reservoir = rOther;
        reservoir.jacobian = jacobianOther;
        reservoir.jacobianOutgoing = jacobianOtherOut;
        reservoir.F = Fother;
    }

    //Update c and weight
    reservoir.weight = wSum;
    reservoir.c = cSum;

    //Finalize Reservoir
    float sampleRadiance = luminance(reservoir.F);
    if (reservoir.weight > 0 && sampleRadiance > 0)
        reservoir.weight /= sampleRadiance;
    else
        reservoir.weight = 0;

    return true;
}

[numthreads(16, 16, 1)]
void main(uint2 pixel: SV_DispatchThreadID)
{
    if (any(pixel >= gFrameDim))
        return;

    //Get current pixels hit and initialize reservoir
    HitInfo hit = HitInfo(gVBuffer[pixel]);
    Reservoir reservoir = Reservoir();

    //If hit is valid, perform resampling
    if (hit.isValid()) {
        let lod = ExplicitLodTextureSampler(0.f);

        // Sample generator for the pixel offset (search region). Is the same as for the shift in the other shader (TraceCamera.slang)
        SampleGenerator sgPixelOffset = SampleGenerator(pixel, gReservoirOffsetSGSeed);
        // Sample generator for bsdf evaluation
        SampleGenerator sg = SampleGenerator(pixel, gSGSeed);

        //Determine pixel for other reservoir       
        bool isSpatial = gNumResamplingPass > 0;
        float2 otherPixelF = float2(pixel) + 0.5;
        //Add motion vectors for temporal resampling
        if (!isSpatial)
        {
            float2 motionOffset = gMVec[pixel] * float2(gFrameDim);
            otherPixelF += motionOffset;
        }
        float2 u = sampleNext2D(sgPixelOffset);
        if (isSpatial) // Spatial, sample in disc around current pixel
        {
            float2 spatialOffset = gSpatialRadius * sample_disk_concentric(u);
            otherPixelF += spatialOffset;
        } else // Temporal, add subpixel offset
        {
            otherPixelF += u - 0.5; // [-0.5, 0.5]
        }
        int2 otherPixel = int2(otherPixelF);

        //Check if reservoir is in screen
        bool resamplingValid = any(otherPixel >= 0) || any(otherPixel < gFrameDim);
        //Load Shading Data of other reservoir
        ShadingData sdOther;
        if (resamplingValid) {
            HitInfo hitOther;
            float3 viewOther;
            if (isSpatial)
            {
                hitOther = HitInfo(gVBuffer[otherPixel]);
                viewOther = gView[otherPixel].xyz;
            }
            else
            {
                hitOther = HitInfo(gVBufferPrev[otherPixel]);
                viewOther = gViewPrev[otherPixel].xyz;
            }

            if (hitOther.isValid())
                sdOther = loadShadingData(hitOther, viewOther, lod);
            else
                resamplingValid = false;
        }

        // Load current reservoir and begin resampling
        Reservoir rCurr;
        if (isSpatial)
            rCurr = gPathReservoirOther[index2Dto1D(pixel, gFrameDim.x)];
        else
            rCurr = gPathReservoir[index2Dto1D(pixel, gFrameDim.x)];

        //Load other reservoir and current shading data and begin resampling
        if (resamplingValid)
        {
            ShadingData sdCurr = loadShadingData(hit, gView[pixel].xyz, lod);

            Reservoir rOther = gPathReservoirOther[index2Dto1D(otherPixel, gFrameDim.x)];
            resamplingValid = resampleReservoir(reservoir, rCurr, rOther, pixel, sdCurr, sdOther, sg);
        }

        //If resampling was not valid, use the current reservoir
        if (!resamplingValid)
            reservoir = rCurr;
    }

    //Store reservoir
    gPathReservoir[index2Dto1D(pixel, gFrameDim.x)] = reservoir;   
}
