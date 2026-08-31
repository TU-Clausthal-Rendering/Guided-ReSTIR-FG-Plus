#pragma once
#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.RaytracingInline;
import Utils.Sampling.SampleGenerator;
import Utils.Color.ColorHelpers;
import Utils.Math.MathHelpers;
import Rendering.Lights.EmissiveLightSampler;
import Rendering.Lights.EmissiveLightSamplerHelpers;
import StructsAndHelpers;
import NextEventEstimationHelper;
//These resources are only needed for initial sample generation
#if !IS_SHIFT
import Rendering.RTXDI.RTXDI;
#endif

//Shared I/O for generation and shift
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gView;

//Pass specific I/O
#if IS_SHIFT
    Texture2D<float2> gMVec;
    RWTexture2D<float4> gShiftData;
    StructuredBuffer<PathReservoir> gPathReservoir;
#else
    RWStructuredBuffer<PathReservoir> gPathReservoir;
    //Photon Collection
    StructuredBuffer<PhotonData> gPhotonData[2];
    StructuredBuffer<AABB> gPhotonAABB[2];
    StructuredBuffer<PhotonGuidingData> gPhotonGuidingData[2];
    RaytracingAccelerationStructure gPhotonAS;
#endif

cbuffer CB
{
    uint gFrameCount;              // Frame count since scene was loaded.
    float3 gNeeSelectProbabilites; //(Emissive/Analytic/EnvMap)Light selection probabilities for NEE samples.

    uint gMaxPathLength; // Max path length for a Final Gather sample
    float gJacobianDistanceThreshold; // Jacobian distance threshold. Connection can only be performed if distance is greater
}

//Additonal shift data
#if IS_SHIFT
cbuffer ShiftCB
{
    uint gNumResamplingPass;            // Identifier for the resampling pass
    float gSpatialSampleRadius;         // Spatial resample radius
}
#endif

// Shared defines
static const uint kMIHints = (uint)MaterialInstanceHints::AdjustShadingNormal;
static const bool kEvalDeltaPdfs = EVAL_DELTA_PDFS;
static const bool kUseEnvMap = USE_ENV_BACKGROUND;
static const bool kDiffClassUseLobes = DIFF_CLASS_USE_LOBES;
static const bool kUsePhotonGuiding = USE_PHOTON_GUIDING;
static const bool kAlphaTest = ENABLE_ALPHA_TEST;
static const uint kRNGNumPasses = RNG_NUM_PASSES;
static const float kRoughnessThreshold = ROUGHNESS_THRESHOLD;
static const bool kUseEnvBackground = USE_ENV_BACKGROUND;

// Photon Collection Defines
static const float kPhotonCollectTMax = 1e-7;
static const uint kPhotonInstanceMaskGlobal = 0x01;
static const uint kPhotonInstanceMaskCaustic = 0x02;
static const uint kPhotonInstanceMaskAll = 0xFF;
static const uint kPhotonTypeIndexCaustic = 1;

// Defines Shift
static const bool kIsShift = IS_SHIFT;
static const bool kShiftIsCurrent = SHIFT_IS_CURRENT;   //True if the working reservoir is the shifted one

struct RayPayload
{
    PackedHitInfo packedHitInfo; //< Hit Info

    /** Create ray payload with default parameters.
     */
    __init()
    {
        this.packedHitInfo = {};
    }
}

//PathFlags
enum PathFlags : uint {
    deltaPath = 0x100,                  // 1<<8
    lastHitReconnectionSurface = 0x200, // 1<<9
    specularPath = 0x400,               // 1<<10
    lastHitDiffuse = 0x800,             // 1<<11
    lastHitNee = 0x1000,                // 1<<12
    unused0 = 0x2000,                   // 1<<13
    unused1 = 0x4000,                   // 1<<14
    unused2 = 0x8000,                   // 1<<15
}

//Struct with all needed path data
struct PathData {
    SampleGenerator dirSG;  // Sample Generator used for Random Replay
    SampleGenerator sg;     // Sample Generator used for everything else

    float3 thp;             // Throughput
    float scatterPDF;       // Scatter pdf for last hit

    float3 prevPosW;        // PosW from last iteration
    uint flags;             // Path Lenght and Flags

    float3 prevFaceN;       // Face Normal from last iteration
    bool _pad;              // Padding

#if IS_SHIFT
    float3 F;               // Accumulated path radiance
    float jacobian;         // Jacobian used in retracing
#endif

    __init() {
        thp = float3(1);
        scatterPDF = 0;
        flags = 0;
        deltaPath = true;
        specularPath = true;
        prevPosW = float3(0);
        prevFaceN = float3(0);
        _pad = 0;
    #if IS_SHIFT
        F = float3(0);   
        jacobian = 0;
    #endif
    }

    property uint pathLength {
        get { return flags & 0xFF; }
        set {
            flags &= ~0xFF; // Clear lower bits
            flags |= (newValue & 0xFF);
        }
    }

    property bool deltaPath {
        get { return (flags & (uint)PathFlags::deltaPath) > 0; }
        set {
            flags &= ~(uint)PathFlags::deltaPath; // Clear bit
            flags |= (newValue ? (uint)PathFlags::deltaPath : 0);
        }
    }

    property bool lastHitReconnectionSurface {
        get { return (flags & (uint)PathFlags::lastHitReconnectionSurface) > 0; }
        set {
            flags &= ~(uint)PathFlags::lastHitReconnectionSurface; // Clear bit
            flags |= (newValue ? (uint)PathFlags::lastHitReconnectionSurface : 0);
        }
    }

    property bool specularPath {
        get { return (flags & (uint)PathFlags::specularPath) > 0; }
        set {
            flags &= ~(uint)PathFlags::specularPath; // Clear bit
            flags |= (newValue ? (uint)PathFlags::specularPath : 0);
        }
    }

    property bool lastHitDiffuse {
        get { return (flags & (uint)PathFlags::lastHitDiffuse) > 0; }
        set {
            flags &= ~(uint)PathFlags::lastHitDiffuse; // Clear bit
            flags |= (newValue ? (uint)PathFlags::lastHitDiffuse : 0);
        }
    }

    property bool lastHitNee {
        get { return (flags & (uint)PathFlags::lastHitNee) > 0; }
        set {
            flags &= ~(uint)PathFlags::lastHitNee; // Clear bit
            flags |= (newValue ? (uint)PathFlags::lastHitNee : 0);
        }
    }
}

//Photon collection specific functions. Only needed for sample generation
#if !IS_SHIFT

//Photon Sample 
struct PhotonSample {
    float3 posW;
    float radius;

    float3 F;
    uint photonType;

    uint2 packedPhotonRetraceData;    // Packed sample generator for photon retrace (uint2(PackedPixel,Seed); see PhotonData)
    uint2 packedGuidingData; // Packed data needed for guiding. Unused if guiding is disabled

    float invPhotonScaleFactor;
    uint _pad;

    __init() {
        posW = float3(0);
        radius = 0;
        photonType = 0;
        F = float3(0);
        packedPhotonRetraceData = uint2(0);
        packedGuidingData = uint2(0);
        invPhotonScaleFactor = 1.0;
        _pad = 0;
    }
}

// Checks if the ray start point is inside the sphere.
bool hitSphere(const float3 center, const float radius, const float3 p)
{
    float3 radiusTest = p - center;
    return dot(radiusTest, radiusTest) < radius * radius;
}

//Performes photon collection using the ray tracing acceleration structure
//RIS is used to select one representative photon
void photonRIS(ShadingData sd, IMaterialInstance mi, float3 N, bool collectOnlyCaustics, inout SampleGenerator sg, out PhotonSample photonSample, bool collectOnlyDirectGlobal = false) {
    photonSample = PhotonSample();
    RayDesc ray;
    ray.Origin = sd.posW;
    ray.Direction = sd.faceN;
    ray.TMin = 0;
    ray.TMax = kPhotonCollectTMax;

    uint instanceMask = collectOnlyCaustics ? kPhotonInstanceMaskCaustic : kPhotonInstanceMaskAll;

    float weight = 0;
    float choosenApproxF = 0;
    RayQuery<RAY_FLAG_NONE> rayQuery;
    rayQuery.TraceRayInline(gPhotonAS, RAY_FLAG_NONE, instanceMask, ray);
    while (rayQuery.Proceed()) {

        if (rayQuery.CandidateType() != CANDIDATE_PROCEDURAL_PRIMITIVE)
            continue;
        const uint instanceIdx = rayQuery.CandidateInstanceIndex();
        const uint primIdx = rayQuery.CandidatePrimitiveIndex();

        // Check if ray is inside photon radius
        AABB photonAABB = gPhotonAABB[instanceIdx][primIdx];
        float3 photonPosW = photonAABB.center();
        float radius = (photonAABB.maxPoint.x - photonAABB.minPoint.x) / 2.0;
        if (!hitSphere(photonPosW, radius, sd.posW))
            continue;

        PhotonData photonData = gPhotonData[instanceIdx][primIdx];
        PhotonGuidingData pgd = PhotonGuidingData();
        if (kUsePhotonGuiding)
            pgd = gPhotonGuidingData[instanceIdx][primIdx];

        // If direct global photons should be collected and the photon is global, check the path length
        if (collectOnlyDirectGlobal)
        {
            if (((PhotonType)instanceIdx == PhotonType::Global) && (photonData.pathLenght > 0))
                continue;
        }

        float NdotL = dot(photonData.dir, N);
        // Check for diffent surfaces (e.g. corners)
        if ((dot(photonData.faceN, sd.faceN) < 0.6) || (NdotL <= 0))
            continue;

        float3 bsdf = mi.eval(sd, photonData.dir, sg);
        float3 F = max(0, (photonData.flux * bsdf) / (M_PI * radius * radius * NdotL)); // NdotL needs to be taken out of the bsdf for Photon Mapping
        float approxF = luminance(F);
        weight += approxF;
        bool selected = (sampleNext1D(sg) * weight) < approxF;
        if (selected) {
            choosenApproxF = approxF;
            photonSample.posW = photonPosW;
            photonSample.radius = radius;
            photonSample.F = F;
            photonSample.photonType = instanceIdx;
            photonSample.packedPhotonRetraceData = photonData.packPhotonRetraceData();
            photonSample.packedGuidingData = pgd.pack();
            photonSample.invPhotonScaleFactor = photonData.invPhotonScaleFactor;
        }
    }

    // Finalize RIS
    if (choosenApproxF > 0)
    {
        weight /= choosenApproxF;
        photonSample.F *= weight;
    }
}

#endif //!IS_SHIFT


/* Samples new direction and sets flags
   \param[in] Shading data of the current surface
   \param[in] MaterialInstance of the current surface
   \param[inout] PathData of the path
   \param[inout] Ray for the new direction
   \return true if sampling was succesfull
*/
bool sampleNewDirection(ShadingData sd, IMaterialInstance mi, inout PathData pd, inout RayDesc ray) {
    // Photon Bounce for next iteration
    BSDFSample bsdfSample;
    bool validSample = mi.sample(sd, pd.dirSG, bsdfSample, true /*ImportanceSampling*/);
    if (validSample) {
        const bool deltaLobe = bsdfSample.isLobe(LobeType::Delta);
        const bool transmission = bsdfSample.isLobe(LobeType::Transmission);
        const bool disableDeltaPdf = deltaLobe && !kEvalDeltaPdfs; // If true uses 1 for pdf

        //Update Ray
        ray.Origin = sd.computeNewRayOrigin(!transmission); // false is for transmissions
        ray.Direction = bsdfSample.wo;

        //Update Path Data
        pd.thp *= bsdfSample.weight;
        pd.scatterPDF = disableDeltaPdf ? 1.0 : bsdfSample.pdf;
        pd.deltaPath &= deltaLobe;  //Is relevant for RTXDI 
        //If lobes are used, update specular here
        if (kDiffClassUseLobes)
            pd.specularPath &= bsdfSample.isLobe(LobeType::Specular) || deltaLobe;
    }
  
    return validSample;
}

/* Add sample to reservoir
    \param[inout] reservoir Reservoir to add the sample to.
    \param[in] rand Random number in [0,1).
    \param[in] radiance Radiance of the light sample or photon sample
    \param[in] thp Throughput until the reconnection surface.
    \return true if sample was selected as new representative sample
*/
bool addSampleToReservoir(inout PathReservoir reservoir, float rand, float3 radiance, float3 thp)
{
    float3 F = radiance * thp;
    float w = luminance(F);
    reservoir.weight += w;
    bool selectOther = rand * reservoir.weight <= w;
    if (selectOther) {
        reservoir.F = F;
        reservoir.fgRadiance = radiance;
    }
    return selectOther;
}

//Traces a path, starting from the first surface visible from camera
void tracePath(RayDesc ray, inout PathData pd, inout PathReservoir pathReservoir)
{
    // If first reflection was delta, it is a delta path
    const bool firstHitIsDelta = pd.deltaPath;
    //Ray Tracing Loop
    RayPayload rayPayload = RayPayload();
    const uint maxPathLength = kIsShift ? pathReservoir.pathLength : gMaxPathLength;
    for (pd.pathLength = 1; pd.pathLength <= maxPathLength; pd.pathLength = pd.pathLength + 1)
    {
        // Trace ray and get surface info
        TraceRay(gScene.rtAccel, 0 /*Flags*/, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayPayload);
        HitInfo hit = HitInfo(rayPayload.packedHitInfo);

        // Scene miss (evaluate enviroment map). Path Lengh == 1 is covered by ReSTIR DI except the primary hit is delta
        if (!hit.isValid())
        {
            bool validEnvMapSample = (pd.pathLength > 1) || (pd.pathLength == 1 && pd.deltaPath);
            if (kIsShift) //Additionally check if reservoir took the same path
                validEnvMapSample &= pathReservoir.flags.isEnvMapHit && (pd.pathLength == pathReservoir.pathLength);
            if (validEnvMapSample)
            {
                float misWeight = 1;
                // If last hit could have generated a Enviroment sample, calculate MIS weight
                if (gNeeSelectProbabilites[kNeeLightTypeEnvMap] > 0 && pd.lastHitNee)
                {
                    float lightPdf = gNeeSelectProbabilites[kNeeLightTypeEnvMap] * gEnvMapSampler.evalPdf(ray.Direction);
                    misWeight = evalMIS(1, pd.scatterPDF, 1, lightPdf);
                }
                float3 envMapContribution = kUseEnvMap ? gScene.envMap.eval(ray.Direction) * misWeight : float3(0);

                //Either store shift data or add the sample to the reservoir
                #if IS_SHIFT
                    pd.F = envMapContribution * pd.thp;
                    pd.jacobian = pd.scatterPDF;
                #else
                    if (addSampleToReservoir(pathReservoir, sampleNext1D(pd.sg), envMapContribution, pd.thp))
                    {
                        pathReservoir.jacobian = pd.scatterPDF;
                        uint flags = (uint)ReservoirPathFlags::hasNoReconnectionSurface | (uint)ReservoirPathFlags::isEnvMapHit;
                        flags |= firstHitIsDelta ? (uint)ReservoirPathFlags::firstHitIsDelta : 0;
                        pathReservoir.flags.setReservoirPathFlags(flags);
                    }
                #endif
            }
            break; //End Loop
        }

        // Load Material Information
        let lod = ExplicitLodTextureSampler(0);
        ShadingData sd = loadShadingData(hit, -ray.Direction, lod);
        let mi = gScene.materials.getMaterialInstance(sd, lod, kMIHints);
        let bsdfProperties = mi.getProperties(sd);
        float rayDist = distance(ray.Origin, sd.posW);
        bool hitIsEmissive = any(bsdfProperties.emission > 0);
        float jacobian = pd.scatterPDF * abs(dot(ray.Direction, sd.faceN)) / (rayDist * rayDist); // Jacobian for this iteration
        //Diffuse Surface and Reconnection Surface
        bool jacobianDistanceThreshold = rayDist > gJacobianDistanceThreshold;
        bool roughnessThreshold = bsdfProperties.roughness > kRoughnessThreshold;
        bool hitIsDiffuse = roughnessThreshold;
        if (kDiffClassUseLobes)
            hitIsDiffuse |= (mi.getLobeTypes(sd) & (uint)LobeType::Diffuse) > 0;
        bool hitIsReconnectionSurface = hitIsDiffuse && jacobianDistanceThreshold && roughnessThreshold;

        // Handle Emissive Hit. The sample is evaluated if the whole path has diffuse surfaces to
        // collect photons or if NEE was used in the last iteration.
        // Path length == 1 is covered by RTXDI, except when the first hit was delta
        bool validEmissiveHitSample = hitIsEmissive;
        validEmissiveHitSample &= pd.pathLength > 1 && pd.specularPath || (pd.pathLength == 1 && pd.deltaPath) || pd.lastHitNee;
        if (kIsShift)
            validEmissiveHitSample &= pathReservoir.flags.isEmissiveHit && (pd.pathLength == pathReservoir.pathLength);
        if (validEmissiveHitSample)
        {
            float misWeight = 1.0;
            float neeEmissiveProbability = gNeeSelectProbabilites[kNeeLightTypeEmissive];
            if (neeEmissiveProbability > 0 && pd.lastHitNee)
            {
                // Get hit in light collection representation
                TriangleHit triangleHit = hit.getTriangleHit();
                TriangleLightHit lightHit;
                lightHit.triangleIndex = gScene.lightCollection.getTriangleIndex(triangleHit.instanceID, triangleHit.primitiveIndex);
                lightHit.posW = sd.posW;
                lightHit.normalW = sd.getOrientedFaceNormal();
                float neePDF = neeEmissiveProbability * gEmissiveSampler.evalPdf(pd.prevPosW, pd.prevFaceN, true, lightHit);
                misWeight = evalMIS(1.0, pd.scatterPDF, 1.0, neePDF);
            }
            float3 emissiveContribution = bsdfProperties.emission * misWeight;

            // Either store shift data or add the sample to the reservoir
            #if IS_SHIFT
                pd.F = emissiveContribution * pd.thp;
                pd.jacobian = jacobian;
            #else //IS_SHIFT
                if (addSampleToReservoir(pathReservoir, sampleNext1D(pd.sg), emissiveContribution, pd.thp))
                {
                    pathReservoir.jacobian = jacobian;
                    uint flags = (uint)ReservoirPathFlags::hasNoReconnectionSurface | (uint)ReservoirPathFlags::isEmissiveHit;
                    flags |= firstHitIsDelta ? (uint)ReservoirPathFlags::firstHitIsDelta : 0;
                    pathReservoir.flags.setReservoirPathFlags(flags);
                    pathReservoir.pathLength = pd.pathLength;
                }
            #endif //IS_SHIFT
        }

        //Stop on emissive hits, as reflection is undefined
        if (hitIsEmissive)
            break;

        // If the surface is diffuse, do radiance estimate and evaluate NEE
        pd.lastHitNee = false; //Reset flag
        if (hitIsDiffuse)
        {
            //If the surface is a reconnection candidate
            const bool isReconnectionSurface = pd.lastHitReconnectionSurface && hitIsReconnectionSurface;
            // Use final gather, if this is the secound diffuse surface that is enounted on the path
            // Can be classified as "Radiance Estimate" if it is no reconnection surface
            const bool isFinalGather = !pd.specularPath || isReconnectionSurface;

            if (isFinalGather && !isReconnectionSurface)
                sd.mtl.setActiveLobes((uint)LobeType::Diffuse);

            // Either check if reservoir sample is valid, or perform radiance estimate and NEE (on the first diffuse hit)
            #if IS_SHIFT
                if (pd.pathLength == pathReservoir.pathLength)
                {
                    float3 toSample = float3(0);
                    float distToSample = -1;
                    bool isFinalGatherOrNEE = false;
                    if (pathReservoir.flags.isFinalGather)
                    {
                        //Get PDF and to sample info for new jacobian
                        toSample = pathReservoir.samplePosW - sd.posW;
                        distToSample = length(toSample);
                        toSample /= distToSample;
                        float pdf = mi.evalPdf(sd, toSample);
                        // Update jacobian and F
                        pd.jacobian = abs(dot(pathReservoir.sampleNormal, -toSample)) / (distToSample * distToSample);
                        pd.jacobian *= pdf;
                        pd.F = pathReservoir.fgRadiance / pdf; // BSDF and thp are added later
                        isFinalGatherOrNEE = any(pd.F > 0);
                    } else if (pathReservoir.flags.isRadianceEstimate)
                    {
                        bool isReconnectable = distance(pathReservoir.samplePosW, sd.posW) < pathReservoir.sampleRadius;
                        if (isReconnectable) {
                            pd.F = pathReservoir.fgRadiance * pd.thp;
                            pd.jacobian = jacobian;
                        }
                    } else if (pathReservoir.flags.isNEE)
                    {
                        SampleGenerator sg = SampleGenerator(pathReservoir.sampleData); // Recreate sample generator
                        //Recreate light sampling
                        LightSample ls;
                        bool validLightSample = generateAndSelectLightSample(sd, gNeeSelectProbabilites, ls, sg);
                        if (validLightSample && ls.pdf > 0)
                        {
                            //Jacobian
                            pd.jacobian = ls.pdf;
                            if (!ls.isInfiniteDistance) {
                                pd.jacobian *= abs(dot(ls.faceN, ls.dir)) / (ls.distance * ls.distance);
                            }
                            // MIS
                            float misWeight = 1;
                            if (ls.type != kNeeLightTypeAnalytic) {
                                float scatterPdf = mi.evalPdf(sd, ls.dir);
                                misWeight = evalMIS(1.0, ls.pdf, 1.0, scatterPdf);
                            }

                            pd.F = ls.Li * misWeight; // BSDF and thp is added later
                            toSample = ls.dir;
                            distToSample = ls.distance;
                            isFinalGatherOrNEE = any(pd.F > 0);
                        }
                    }

                    // Add BSDF and thp for Final Gather and NEE
                    if (isFinalGatherOrNEE)
                    {
                        float3 bsdf = mi.eval(sd, toSample, pd.sg);
                        pd.F *= bsdf * pd.thp;
                        if (!dispatchShadowRay(sd, toSample, distToSample))
                            pd.F = float3(0);
                    }
                    break;
                }
            #else //IS_SHIFT, Collect photons and do NEE
                //Collect Photons
                PhotonSample photonSample;
                photonRIS(sd, mi, bsdfProperties.guideNormal, !isFinalGather, pd.sg, photonSample);
                if (addSampleToReservoir(pathReservoir, sampleNext1D(pd.sg), photonSample.F, pd.thp))
                {
                    pathReservoir.pathLength = isReconnectionSurface ? pd.pathLength - 1 : pd.pathLength;
                    pathReservoir.jacobian = jacobian;
                    pathReservoir.sampleRadius = photonSample.radius;
                    uint flags = 0;
                    if (photonSample.photonType == kPhotonTypeIndexCaustic)
                        flags |= (uint)ReservoirPathFlags::isCaustic;
                    pathReservoir.packedPhotonRetraceData = photonSample.packedPhotonRetraceData;
                    pathReservoir.invPhotonScaleFactor = photonSample.invPhotonScaleFactor;
                    pathReservoir.packedGuidingData = photonSample.packedGuidingData;

                    if (isReconnectionSurface) //Final Gather with Reconnection
                    {
                        flags |= (uint)ReservoirPathFlags::isFinalGather;
                        pathReservoir.samplePosW = sd.posW;
                        pathReservoir.sampleNormal = sd.faceN;
                    } else //Radiance Estimate
                    {
                        flags |= (uint)ReservoirPathFlags::hasNoReconnectionSurface | (uint)ReservoirPathFlags::isRadianceEstimate;
                        pathReservoir.samplePosW = photonSample.posW;
                        pathReservoir.sampleRadius = photonSample.radius;
                    }
                    flags |= firstHitIsDelta ? (uint)ReservoirPathFlags::firstHitIsDelta : 0;
                    pathReservoir.flags.setReservoirPathFlags(flags);
                }

                // If only caustics where calculated, also do NEE
                if (!isFinalGather)
                {
                    LightSample ls;
                    uint4 sgState = pd.sg.getState();
                    float3 nee = nextEventEstimation(sd, mi, gNeeSelectProbabilites, ls, pd.sg);
                    if (addSampleToReservoir(pathReservoir, sampleNext1D(pd.sg), nee, pd.thp))
                    {
                        pathReservoir.jacobian = ls.pdf;
                        if (!ls.isInfiniteDistance) {
                            pathReservoir.jacobian *= abs(dot(ls.faceN, ls.dir)) / (ls.distance * ls.distance);
                        }
                        pathReservoir.sampleData = sgState;
                        uint flags = (uint)ReservoirPathFlags::hasNoReconnectionSurface | (uint)ReservoirPathFlags::isNEE;
                        flags |= firstHitIsDelta ?(uint)ReservoirPathFlags::firstHitIsDelta : 0;
                        pathReservoir.flags.setReservoirPathFlags(flags);
                    }
                    pd.lastHitNee = true;
                }
            #endif //IS_SHIFT

            //Path is stopped after final gather
            if (isFinalGather)
                break;
        }

        //Update flags
        pd.lastHitDiffuse = hitIsDiffuse;
        pd.lastHitReconnectionSurface = hitIsReconnectionSurface;
        if(!kDiffClassUseLobes) //For threshold, lobes are evaluated in sampleNewDirection
            pd.specularPath &= !hitIsDiffuse;
        pd.prevPosW = sd.posW;
        pd.prevFaceN = sd.faceN;

        //Sample new ray direction
        if (!sampleNewDirection(sd, mi, pd, ray))
            break;
    }

    // Finalize Reservoir
    if (any(pathReservoir.F > 0))
        pathReservoir.weight /= luminance(pathReservoir.F);
}

[shader("miss")]
void miss(inout RayPayload rayData: SV_RayPayload)
{
    HitInfo hitInfo = {};
    rayData.packedHitInfo = hitInfo.getData(); // Invalid hit
}

//For alpha test
[shader("anyhit")]
void anyHit(inout RayPayload rayData: SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs: SV_IntersectionAttributes)
{
    if (kAlphaTest)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}

[shader("closesthit")]
void closestHit(inout RayPayload rayData: SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs: SV_IntersectionAttributes) {
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;

    rayData.packedHitInfo = HitInfo(triangleHit).getData();
}

// Sets surface data for the VBuffer hit.
// It sets the diffuse surface flags, depending on the classification and previous PosW and FaceN
inline void setFirstHitSurfaceData(inout PathData pd, const ShadingData sd, const IMaterialInstance mi, const BSDFProperties bsdfProperties)
{
    bool roughnessThreshold = bsdfProperties.roughness > kRoughnessThreshold;
    if (kDiffClassUseLobes) //BSDF Lobes
    {
        pd.lastHitDiffuse = (mi.getLobeTypes(sd) & (uint)LobeType::Diffuse) > 0;
        pd.lastHitReconnectionSurface = pd.lastHitDiffuse && roughnessThreshold;
    } else //Roughness Threshold
    {
        pd.lastHitDiffuse = roughnessThreshold;
        pd.lastHitReconnectionSurface = roughnessThreshold;
        pd.specularPath &= !roughnessThreshold;
    }

    //Set prev pos and faceN
    pd.prevPosW = sd.posW;
    pd.prevFaceN = sd.faceN;
}

#if IS_SHIFT
// Fetches temporal/spatial reservoir and performs retracing if neccessary 
void shiftCameraReservoir()
{
    int2 pixel = DispatchRaysIndex().xy;
    uint2 dispatchDims = DispatchRaysDimensions().xy;

    // Init Sample Generator for selecting the reservoir
    SampleGenerator sgSelectReservoir = SampleGenerator(pixel, gFrameCount * kRNGNumPasses + 5 + (gNumResamplingPass * 3));

    // Reproject last frames reservoir (temporal) or randomly sample in a disc (spatial)
    bool isSpatial = gNumResamplingPass > 0;
    float2 reservoirPixelF = float2(pixel) + 0.5; // Start from pixel center
    float2 u = sampleNext2D(sgSelectReservoir); //Random value
    if (isSpatial)
    {
        float2 spatialOffset = gSpatialSampleRadius * sample_disk_concentric(u);
        reservoirPixelF += spatialOffset;
    } else //Temporal
    {
        float2 motionOffset = gMVec[pixel] * float2(dispatchDims);
        reservoirPixelF += motionOffset + (u - 0.5); //Motion + random subpixel offset
    }
    int2 reservoirPixel = int2(reservoirPixelF);

    // If reservoir pixel is outside of the screen, it will not be selected during resampling either
    if (any(reservoirPixel < 0) || any(reservoirPixel >= dispatchDims))
        return;

    // If the working reservoir is the one beeing shifted, swap pixel and reservoir index
    if (kShiftIsCurrent)
    {
        int2 tmp = reservoirPixel;
        reservoirPixel = pixel;
        pixel = tmp;
    }

    // Get Surface, view and reservoir
    HitInfo hitInfo = HitInfo(gVBuffer[pixel]);
    float3 viewDir = gView[pixel].xyz;
    PathReservoir pathReservoir = gPathReservoir[index2Dto1D(reservoirPixel, dispatchDims.x)];

    // Swap back the write index
    if (kShiftIsCurrent)
        pixel = reservoirPixel;

    // When first hit is valid
    float4 shiftData = float4(0);
    if (hitInfo.isValid() && pathReservoir.isValid() && pathReservoir.pathLength >= 1)
    {
        //Init Path Data struct
        PathData pd = PathData();
        pd.sg = SampleGenerator(pixel, gFrameCount * kRNGNumPasses + 6 + (gNumResamplingPass * 3)); // BSDF SG
        pd.dirSG = pathReservoir.sgPathDirections;
        //Init first hit surface
        let lod = ExplicitLodTextureSampler(0);
        ShadingData sd = loadShadingData(hitInfo, viewDir, lod);
        let mi = gScene.materials.getMaterialInstance(sd, lod, kMIHints);
        let bsdfProperties = mi.getProperties(sd);

        //Prepare first hit data
        setFirstHitSurfaceData(pd, sd, mi, bsdfProperties);

        //Init ray and (re)trace path
        RayDesc ray = {};
        ray.TMin = 0;
        ray.TMax = FLT_MAX;
        if (sampleNewDirection(sd, mi, pd, ray))
            tracePath(ray, pd, pathReservoir);

        shiftData = float4(pd.F, pd.jacobian);
    }

    gShiftData[pixel] = shiftData;
}
#else // IS_SHIFT

/* Creates a camera ray and initializes the reservoir
 */
void generateCameraRay()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 dispatchDims = DispatchRaysDimensions().xy;

    HitInfo hitInfo = HitInfo(gVBuffer[pixel]);
    float3 viewDir = gView[pixel].xyz;
    PathReservoir pathReservoir = PathReservoir();

    // When first hit is valid
    if (hitInfo.isValid()) {
        // Init Materials and Sample Generators
        PathData pd = PathData();
        let lod = ExplicitLodTextureSampler(0);
        pd.sg = SampleGenerator(pixel, gFrameCount * kRNGNumPasses + 2);
        // Sample generator for sampling new directions, needed in Random Replay
        pd.dirSG = SampleGenerator(pixel, gFrameCount * kRNGNumPasses + 3);
        pathReservoir.sgPathDirections = pd.dirSG;

        ShadingData sd = loadShadingData(hitInfo, viewDir, lod);
        let mi = gScene.materials.getMaterialInstance(sd, lod, kMIHints);
        let bsdfProperties = mi.getProperties(sd);

        // Set RTXDI(ReSTIR DI) surface data
        float distanceToSurface = distance(sd.posW, gScene.camera.getPosition());
        gRTXDI.setSurfaceData(pixel, sd.computeNewRayOrigin(), bsdfProperties.guideNormal, bsdfProperties.diffuseReflectionAlbedo, bsdfProperties.specularReflectance, bsdfProperties.roughness, distanceToSurface);

        //Prepare first hit data
        setFirstHitSurfaceData(pd, sd, mi, bsdfProperties);

        RayDesc ray = {};
        ray.TMin = 0;
        ray.TMax = FLT_MAX;
        if (sampleNewDirection(sd, mi, pd, ray))
            tracePath(ray, pd, pathReservoir);
    } else {
        // Set invalid RTXDI data
        gRTXDI.setInvalidSurfaceData(pixel);
    }

    gPathReservoir[index2Dto1D(pixel, dispatchDims.x)] = pathReservoir;
}
#endif // IS_SHIFT


[shader("raygeneration")]
void rayGen()
{
    #if IS_SHIFT
        shiftCameraReservoir();
    #else
        generateCameraRay();
    #endif

}
