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
    StructuredBuffer<Reservoir> gPathReservoir;

    RWTexture2D<float4> gShiftData;
    RWStructuredBuffer<ShiftedSurface> gShiftSurface;
    RWTexture2D<float4> gShiftedRcThp;  
#else
    RWStructuredBuffer<Reservoir> gPathReservoir;
#endif

#if SHIFT_RECONNECTION_SAMPLE
    RWTexture2D<float4> gShiftRcThp;
#endif

cbuffer CB
{
    uint gSGSeed;              // Sample Generator seed
    float3 gNeeSelectProbabilites;    //(Emissive/Analytic/EnvMap)Light selection probabilities for NEE samples.
    PackedPathLength gPackedPathLength; //Packed Path Length
    float gJacobianDistanceThreshold; // Jacobian distance threshold. Connection can only be performed if distance is greater
}

//Additonal shift data
#if IS_SHIFT
cbuffer ShiftCB
{
    uint gNumResamplingPass;            // Identifier for the resampling pass
    float gSpatialSampleRadius;         // Spatial resample radius
    uint gReservoirSGSeed;
}
#endif

// Shared defines
static const uint kMIHints = (uint)MaterialInstanceHints::AdjustShadingNormal;
static const bool kEvalDeltaPdfs = EVAL_DELTA_PDFS;
static const bool kUseEnvMap = USE_ENV_BACKGROUND;
static const bool kAlphaTest = ENABLE_ALPHA_TEST;
static const bool kDiffClassUseLobes = DIFF_CLASS_USE_LOBES;
static const float kRoughnessThreshold = ROUGHNESS_THRESHOLD;
static const bool kUseEnvBackground = USE_ENV_BACKGROUND;

// Defines Shift
static const bool kIsShift = IS_SHIFT;
static const bool kShiftIsCurrent = SHIFT_IS_CURRENT; // True if the working reservoir is the shifted one
static const bool kShiftReconnectionSample = SHIFT_RECONNECTION_SAMPLE; //Verifies if the reconnection sample still exist by shifting the sample path

//Ray payload stores packed triangle hit
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
    deltaPath = 0x1,                  // 1<<0
    lastHitRoughnessThreshold = 0x2,  // 1<<1
    specularPath = 0x4,               // 1<<2
    reconnectionSurfaceSet = 0x8,     // 1<<3
    lastHitNee = 0x10,                // 1<<4
    reconLastHitSpecular = 0x20,      // 1<<5
    reconSpecular = 0x40,             // 1<<6
    firstHitIsDelta = 0x80,           // 1<<7
}

//Struct with all needed path data
struct PathData {
    SampleGenerator dirSG;  // Sample Generator used for Random Replay
    SampleGenerator sg;     // Sample Generator used for everything else

    float3 thp;             // Throughput
    float scatterPDF;       // scatter pdf for last hit

    float3 prevPosW;        // PosW from last iteration
    uint flags;             // Path Lenght and Flags

    float3 prevFaceN;       // Face Normal from last iteration
    float jacobianRc;       // Jacobian Reconnection

    float3 rcThp;           // Thp after reconnection
    PackedPathLength packedPathLength; // padding

#if IS_SHIFT
    float3 F;               // Accumulated path radiance
    float jacobian;         // Jacobian used in retracing
    float3 shiftRcThp;
    uint _pad;
#else
    PackedHitInfo reconnectionSurface; 
#endif

    __init() {
        thp = float3(1);
        scatterPDF = 0;
        flags = 0;
        deltaPath = true;
        specularPath = true;
        prevPosW = float3(0);
        prevFaceN = float3(0);
        jacobianRc = 0;
        rcThp = 1;
        packedPathLength = PackedPathLength();
    #if IS_SHIFT
        F = float3(0);
        jacobian = 0;
        shiftRcThp = 0;
        _pad = 0;
    #else
        reconnectionSurface = {};
    #endif
    }

    property uint pathLength {
        get { return packedPathLength.pathLength; }
        set {
            packedPathLength.pathLength = newValue;
        }
    }

    property bool deltaPath {
        get { return (flags & (uint)PathFlags::deltaPath) > 0; }
        set {
            flags &= ~(uint)PathFlags::deltaPath; // Clear bit
            flags |= (newValue ? (uint)PathFlags::deltaPath : 0);
        }
    }

    property bool lastHitRoughnessThreshold {
        get { return (flags & (uint)PathFlags::lastHitRoughnessThreshold) > 0; }
        set {
            flags &= ~(uint)PathFlags::lastHitRoughnessThreshold; // Clear bit
            flags |= (newValue ? (uint)PathFlags::lastHitRoughnessThreshold : 0);
        }
    }

    property bool specularPath {
        get { return (flags & (uint)PathFlags::specularPath) > 0; }
        set {
            flags &= ~(uint)PathFlags::specularPath; // Clear bit
            flags |= (newValue ? (uint)PathFlags::specularPath : 0);
        }
    }

    property bool reconnectionSurfaceSet {
        get { return (flags & (uint)PathFlags::reconnectionSurfaceSet) > 0; }
        set {
            flags &= ~(uint)PathFlags::reconnectionSurfaceSet; // Clear bit
            flags |= (newValue ? (uint)PathFlags::reconnectionSurfaceSet : 0);
        }
    }

    property bool lastHitNee {
        get { return (flags & (uint)PathFlags::lastHitNee) > 0; }
        set {
            flags &= ~(uint)PathFlags::lastHitNee; // Clear bit
            flags |= (newValue ? (uint)PathFlags::lastHitNee : 0);
        }
    }

    property bool reconLastHitSpecular {
        get { return (flags & (uint)PathFlags::reconLastHitSpecular) > 0; }
        set {
            flags &= ~(uint)PathFlags::reconLastHitSpecular; // Clear bit
            flags |= (newValue ? (uint)PathFlags::reconLastHitSpecular : 0);
        }
    }

    property bool reconSpecular {
        get { return (flags & (uint)PathFlags::reconSpecular) > 0; }
        set {
            flags &= ~(uint)PathFlags::reconSpecular; // Clear bit
            flags |= (newValue ? (uint)PathFlags::reconSpecular : 0);
        }
    }

    property bool firstHitIsDelta {
        get { return (flags & (uint)PathFlags::firstHitIsDelta) > 0; }
        set {
            flags &= ~(uint)PathFlags::firstHitIsDelta; // Clear bit
            flags |= (newValue ? (uint)PathFlags::firstHitIsDelta : 0);
        }
    }
}

/* Samples new direction and sets flags
   \param[in] Shading data of the current surface
   \param[in] MaterialInstance of the current surface
   \param[inout] PathData of the path
   \param[inout] Ray for the new direction
   \return true if sampling was succesfull
*/
bool sampleNewDirection(ShadingData sd, IMaterialInstance mi, inout PathData pd, inout RayDesc ray, bool reconSetThisIteration = false) {
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
        pd.deltaPath &= deltaLobe; // Is relevant for RTXDI
        if (pd.reconnectionSurfaceSet)
            pd.rcThp *= bsdfSample.weight;

        const bool diffuseLobe = bsdfSample.isLobe(LobeType::Diffuse);
        if (kDiffClassUseLobes)
        {
            if (diffuseLobe)
                pd.lastHitRoughnessThreshold = true;
        }

        if (!pd.reconnectionSurfaceSet)
            pd.reconLastHitSpecular = !diffuseLobe;
        if (reconSetThisIteration)
            pd.reconSpecular = !diffuseLobe;
            
        // Path Lenght
        if (bsdfSample.isLobe(LobeType::Delta))
            pd.packedPathLength.deltaBounces += 1;
        else if (bsdfSample.isLobe(LobeType::Diffuse))
            pd.packedPathLength.diffuseBounces += 1;
        else if (bsdfSample.isLobe(LobeType::Specular))
            pd.packedPathLength.specularBounces += 1;
    }
  
    return validSample;
}

/* Add sample to reservoir
    \param[inout] reservoir Reservoir to add the sample to.
    \param[in] rand Random number in [0,1).
    \param[in] rcThp Throughput after reconnection to light source.
    \param[in] thp Throughput until the reconnection surface.
    \return true if sample was selected as new representative sample
*/
bool addSampleToReservoir(inout Reservoir reservoir, float rand, float3 F, float3 rcThp)
{
    float w = luminance(F);
    reservoir.weight += w;
    bool selectOther = rand * reservoir.weight <= w;
    if (selectOther) {
        reservoir.F = F;
        reservoir.rcThp = rcThp;
    }
    return selectOther;
}

/* Sets reservoir flags depending on path state. Does not set the isNeeAfterRecon flag
    \param[in] flagHitType flag for the sample type (EnvMap, EmissiveHit, Nee).
    \param[in] pd Path Data struct
    \return reservoir flags.
*/
uint setReservoirFlags(uint flagHitType, PathData pd)
{
    uint flags = flagHitType;
    if (pd.reconnectionSurfaceSet)
        flags |= (uint)ReservoirPathFlags::isReconnectionSurface;
        flags |= pd.reconLastHitSpecular ? (uint)ReservoirPathFlags::reconLastHitSpecular : 0;
        flags |= pd.reconSpecular ? (uint)ReservoirPathFlags::reconSpecular : 0;
    flags |= pd.firstHitIsDelta ? (uint)ReservoirPathFlags::firstHitIsDelta : 0;

    return flags;
}

#if IS_SHIFT
    inline void addShiftContribution(inout PathData pd, Reservoir reservoir, float3 emission, float jacobian, bool isReconnectionSurface)
    {
        if (isReconnectionSurface)
        {
            pd.shiftRcThp = emission * pd.rcThp;
        }else
        {
            pd.F = emission * pd.thp;
            pd.jacobian = jacobian;
        }
    }
#endif

//Traces a path, starting from the first surface visible from camera
void tracePath(RayDesc ray, inout PathData pd, inout Reservoir reservoir)
{
    // If first reflection was delta, it is a delta path
    pd.firstHitIsDelta = pd.deltaPath;

    //Set data needed for shifting 
    bool useRcSurface = false; // If true, uses the reconnection surface from the reservoir instead of tracing a hit
    #if IS_SHIFT
        ShiftedSurface shiftedSurface = {};
        bool retraceIsReconnectionSample = false;
        if (kShiftReconnectionSample)
        {
            //If the reconnection surface was the first hit, directly load the reconnection surface instead of tracing a ray
            if (reservoir.rcPathLength == 1 && reservoir.flags.isReconnectionSurface)
            {
                useRcSurface = true;
                retraceIsReconnectionSample = true;
            }
        }
    #endif
   
    //Ray Tracing Loop
    RayPayload rayPayload = RayPayload();
    const uint maxPathLength = kIsShift ? reservoir.pathLength : gPackedPathLength.pathLength + 1;
    for (pd.pathLength = 1; pd.pathLength <= maxPathLength; pd.pathLength = pd.pathLength + 1)
    {
        //Get primary surface
        HitInfo hit = {};
        // Load stored reconnection surface if the whole reconnection path is shifted
        if (useRcSurface) 
        {
            hit = HitInfo(reservoir.reconnectionSurface);
        } else // Trace ray and get surface info
        {
            TraceRay(gScene.rtAccel, 0 /*Flags*/, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayPayload);
            hit = HitInfo(rayPayload.packedHitInfo);
        }

        // Scene miss (evaluate enviroment map). Path Lengh == 1 is covered by ReSTIR DI except the primary hit is delta
        if (!hit.isValid())
        {
            bool validEnvMapSample = (pd.pathLength > 1) || (pd.pathLength == 1 && pd.deltaPath);
            // For shift, check if reservoir took the same path
            if (kIsShift)
                validEnvMapSample &= reservoir.flags.isEnvMapHit && (pd.pathLength == reservoir.pathLength);
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

                // For shift, store the shifted contribution
                // Else, add the sample with RIS to the (initial) reservoir
                #if IS_SHIFT
                    addShiftContribution(pd, reservoir, envMapContribution, pd.scatterPDF, retraceIsReconnectionSample);
                #else
                    if (addSampleToReservoir(reservoir, sampleNext1D(pd.sg), pd.thp * envMapContribution, pd.rcThp * envMapContribution))
                    {
                        reservoir.jacobian = pd.scatterPDF;
                        if (pd.reconnectionSurfaceSet)
                        {
                            reservoir.reconnectionSurface = pd.reconnectionSurface;
                            reservoir.jacobian = pd.jacobianRc;
                        }
                        uint flags = setReservoirFlags((uint)ReservoirPathFlags::isEnvMapHit, pd);
                        reservoir.flags.setReservoirPathFlags(flags);
                        reservoir.pathLength = pd.pathLength;
                    }
                #endif
            }
            break;
        }

        // Load Material information of the hit surface
        bool reconnectionSetThisIteration = false;
        let lod = ExplicitLodTextureSampler(0);
        ShadingData sd;
        // Shading data load uses last hit when the reconnection surface was loaded
        if (useRcSurface)
        {
            sd = loadShadingDataFromOrigin(hit, pd.prevPosW, lod);
            //Mark current hit as reconnection surface
            pd.reconnectionSurfaceSet = true;
            reconnectionSetThisIteration = true;
            useRcSurface = false;
        } else
        {
            sd = loadShadingData(hit, -ray.Direction, lod);
        }
        let mi = gScene.materials.getMaterialInstance(sd, lod, kMIHints);
        let bsdfProperties = mi.getProperties(sd);
        float rayDist = distance(pd.prevPosW, sd.posW);
        const float jacobian = pd.scatterPDF * abs(dot(ray.Direction, sd.faceN)) / (rayDist * rayDist); //Jacobian to the hit surface

        // Handle Emissive Hit.
        // Path length == 1 is covered by RTXDI, except when the first hit was delta
        const bool hitIsEmissive = any(bsdfProperties.emission > 0);
        bool validEmissiveHitSample = hitIsEmissive;
        validEmissiveHitSample &= pd.pathLength > 1 || (pd.pathLength == 1 && pd.deltaPath);
        // For shift, check if reservoir took the same path
        if (kIsShift)
            validEmissiveHitSample &= reservoir.flags.isEmissiveHit && (pd.pathLength == reservoir.pathLength);
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

            // For shift, store the shifted contribution
            // Else, add the sample with RIS to the (initial) reservoir
            #if IS_SHIFT
                addShiftContribution(pd, reservoir, emissiveContribution, jacobian, retraceIsReconnectionSample);
                break;
            #else //IS_SHIFT
                if (addSampleToReservoir(reservoir, sampleNext1D(pd.sg), pd.thp * emissiveContribution, pd.rcThp * emissiveContribution))
                {
                    reservoir.jacobian = jacobian;
                    if (pd.reconnectionSurfaceSet)
                    {
                        reservoir.reconnectionSurface = pd.reconnectionSurface;
                        reservoir.jacobian = pd.jacobianRc;
                    }
                    uint flags = setReservoirFlags((uint)ReservoirPathFlags::isEmissiveHit, pd);
                    reservoir.flags.setReservoirPathFlags(flags);
                    reservoir.pathLength = pd.pathLength;
                }
            #endif //IS_SHIFT
        }

        // Stop on emissive hits (undefined reflection/nee) or if a maximum number of bounces for a path type are reached
        bool invalidPathLength = !pd.packedPathLength.comparePathLengts(gPackedPathLength);
        if (hitIsEmissive && invalidPathLength)
            break;

        // Check if this surface is the reconnection surface
        const bool jacobianDistanceThreshold = rayDist > gJacobianDistanceThreshold;
        bool roughnessThreshold = bsdfProperties.roughness > kRoughnessThreshold;
        if (kDiffClassUseLobes)
            roughnessThreshold |= (mi.getLobeTypes(sd) & (uint)LobeType::Diffuse) > 0;
        const bool isReconnectionSurface = roughnessThreshold && pd.lastHitRoughnessThreshold && jacobianDistanceThreshold;

        // For Shift: Check if the current surface is the hit before the reconnection surface
        // Generation: Set reconnection surface if the current hit is a reconnection surface (roughness, distance and lobe threshold valid for two hits)
        #if IS_SHIFT
            bool reconnectToSample = reservoir.flags.isReconnectionSurface && ((reservoir.rcPathLength - 1) == pd.pathLength);

            if (roughnessThreshold && jacobianDistanceThreshold && reconnectToSample && !pd.reconnectionSurfaceSet)
            {
                //Check if hit has the previously sampled lobe
                bool hasSameLobe = false;
                uint lobes = mi.getLobeTypes(sd);
                if (reservoir.flags.reconLastHitSpecular)
                {
                    hasSameLobe = lobes & ((uint)LobeType::Specular | (uint)LobeType::Delta);
                } else
                {
                    hasSameLobe = lobes & (uint)LobeType::Diffuse;
                }

                if (!hasSameLobe)
                    break;

                shiftedSurface.packedHit = rayPayload.packedHitInfo;
                shiftedSurface.Wi = ray.Direction;
                pd.F = pd.thp;

                //When the whole path of the reconnection sample does not need to be shifted, return here
                if (!kShiftReconnectionSample)
                    break;

                retraceIsReconnectionSample = true;
                useRcSurface = true;
            }
        #else //IS_SHIFT
            //Store hit and mark that the path has a reconnection surface
            if (isReconnectionSurface && !pd.reconnectionSurfaceSet)
            {
                pd.jacobianRc = jacobian; 
                reservoir.rcPathLength = pd.pathLength;
                pd.reconnectionSurface = rayPayload.packedHitInfo;
                pd.reconnectionSurfaceSet = true;
                reconnectionSetThisIteration = true;
            }
        #endif //IS_SHIFT

        // Handle Next Event Estimation (NEE)
        // If the surface has a diffuse or specular lobe, NEE is possible
        bool doNee = (mi.getLobeTypes(sd) & (uint)LobeType::NonDelta) > 0;
        bool doNeeShift = true; //Set to false if this is not the shift node
        if (kIsShift)
        {
            doNeeShift &= reservoir.flags.isNEE && (pd.pathLength == reservoir.pathLength);
            if (reservoir.flags.isNeeAtRecon)
                doNeeShift &= reconnectionSetThisIteration;
        }
        //Perform NEE
        if (doNee && doNeeShift)
        {
            LightSample ls;
            uint4 sgState = pd.sg.getState(); //Store NEE RNG state for random replay
            //If this is the shift, load the RNG for NEE
            if (kIsShift)
                pd.sg = SampleGenerator(reservoir.neeSgSeed);

            float3 nee = nextEventEstimation(sd, mi, gNeeSelectProbabilites, ls, pd.sg);

            // For shift, store the shifted contribution
            // Else, add the sample with RIS to the (initial) reservoir
            #if IS_SHIFT
                float neeJacobian = ls.pdf;
                if (!ls.isInfiniteDistance)
                    neeJacobian *= abs(dot(ls.faceN, ls.dir)) / (ls.distance * ls.distance);
                addShiftContribution(pd, reservoir, nee, neeJacobian, retraceIsReconnectionSample);
                break;
            #else //IS_SHIFT
                if (addSampleToReservoir(reservoir, sampleNext1D(pd.sg), pd.thp * nee, pd.rcThp * nee))
                {
                    reservoir.jacobian = ls.pdf;
                    if (!ls.isInfiniteDistance) {
                        reservoir.jacobian *= abs(dot(ls.faceN, ls.dir)) / (ls.distance * ls.distance);
                    }
                    reservoir.neeSgSeed = sgState;
                    if (pd.reconnectionSurfaceSet)
                    {
                        reservoir.reconnectionSurface = pd.reconnectionSurface;
                        reservoir.jacobian = pd.jacobianRc;
                    }
                    uint flags = setReservoirFlags((uint)ReservoirPathFlags::isNEE, pd);
                    if (reconnectionSetThisIteration)
                        flags |= (uint)ReservoirPathFlags::isNeeAtRecon;
                    reservoir.flags.setReservoirPathFlags(flags);
                    reservoir.pathLength = pd.pathLength;
                }
            #endif //IS_SHIFT
        }
        pd.lastHitNee = doNee; //Needed for MIS

        //Return from shift
        #if IS_SHIFT
        if (pd.pathLength == reservoir.pathLength)
            break;
        #endif

        // Update Path data for next iteration
        pd.lastHitRoughnessThreshold = roughnessThreshold && jacobianDistanceThreshold;
        pd.prevPosW = sd.posW;
        pd.prevFaceN = sd.faceN;

        // Sample new ray direction
        if (!sampleNewDirection(sd, mi, pd, ray, reconnectionSetThisIteration))
            break;

        #if !IS_SHIFT
            //Store outgoing direction and jacobian for reconnection surface 
            if (reconnectionSetThisIteration)
            {
                reservoir.jacobianOutgoing = pd.scatterPDF;
                reservoir.sampleWo = ray.Direction;
            }
        #else
            //When retracing the path of the reconnection surface, set the ray direction to the one stored in the reservoir
            //Note: Normally, the throughput (rcThp) also needs to be adjusted. However, recreating bsdfSample.weight using bsdf and pdf seems bugged and produces a different throughput, therefore it is left out for now.
            if (kShiftReconnectionSample)
            {
                if (reconnectionSetThisIteration)
                {
                    ray.Direction = reservoir.sampleWo;
                }
            }
           
        #endif
    }

    #if !IS_SHIFT
        // Finalize Reservoir
        if (any(reservoir.F > 0))
            reservoir.weight /= luminance(reservoir.F);
    #else
        //Store Shifted reconnection surface and if the whole reconnection path is resampled, also the path throughput
        gShiftSurface[index2Dto1D(DispatchRaysIndex().xy, DispatchRaysDimensions().x)] = shiftedSurface;

        #if SHIFT_RECONNECTION_SAMPLE
            gShiftRcThp[DispatchRaysIndex().xy] = float4(pd.shiftRcThp, 1);
        #endif
#endif
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
    const bool hitIsEmissive = any(bsdfProperties.emission > 0);
    bool roughnessThreshold = bsdfProperties.roughness > kRoughnessThreshold;
    roughnessThreshold &= !hitIsEmissive;
    pd.lastHitRoughnessThreshold = roughnessThreshold;
    pd.specularPath &= !roughnessThreshold;

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
    SampleGenerator sgSelectReservoir = SampleGenerator(pixel, gReservoirSGSeed);

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
    {
        return;
    }

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
    Reservoir pathReservoir = gPathReservoir[index2Dto1D(reservoirPixel, dispatchDims.x)];

    // Swap back the write index
    if (kShiftIsCurrent)
        pixel = reservoirPixel;

    // When first hit is valid
    float4 shiftData = float4(0);
    bool shiftPath = !(pathReservoir.rcPathLength == 1 && pathReservoir.flags.isReconnectionSurface);
    shiftPath |= kShiftReconnectionSample; //If the reconnection sample needs to be retraced, the sample is always shifted
    if (hitInfo.isValid() && pathReservoir.isValid() && shiftPath)
    {
        //Init Path Data struct
        PathData pd = PathData();
        pd.sg = SampleGenerator(pixel, gSGSeed); // BSDF SG
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
    Reservoir reservoir = Reservoir();

    // When first hit is valid
    if (hitInfo.isValid()) {
        // Init Materials and Sample Generators
        PathData pd = PathData();
        let lod = ExplicitLodTextureSampler(0);
        pd.sg = SampleGenerator(pixel, gSGSeed);
        // Sample generator for sampling new directions, needed in Random Replay
        pd.dirSG = SampleGenerator(pixel, gSGSeed + 1);
        reservoir.sgPathDirections = pd.dirSG;

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
            tracePath(ray, pd, reservoir);
    } else //Miss
    {
        // Set invalid RTXDI data
        gRTXDI.setInvalidSurfaceData(pixel);
    }

    gPathReservoir[index2Dto1D(pixel, dispatchDims.x)] = reservoir;
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
