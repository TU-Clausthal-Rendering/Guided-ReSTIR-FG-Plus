#include "Utils/Math/MathConstants.slangh"

import Scene.RaytracingInline;
import Utils.Sampling.SampleGenerator;
import Utils.Color.ColorHelpers;
import Utils.Math.MathHelpers;
import Rendering.Materials.IsotropicGGX;
import Rendering.Materials.Fresnel;

static const float kShadowRayEpsilon = 0.98;
static const float kFalcorDeltaRough = 0.08;    //Falcor threshold for delta lobes. 

//
//Enums
//

enum class PhotonType // : uint32_t
{
    Global = 0,
    Caustic = 1
};

enum ReservoirPathFlags : uint {
    firstHitIsDelta = 0x1000000,             // 1<<24
    isReconnectionSurface = 0x2000000,       // 1<<25
    isEmissiveHit = 0x4000000,               // 1<<26
    isEnvMapHit = 0x8000000,                 // 1<<27
    isNEE = 0x10000000,                      // 1<<28
    reconLastHitSpecular = 0x20000000,       // 1<<29
    reconSpecular = 0x40000000,              // 1<<30
    isNeeAtRecon = 0x80000000,               // 1<<31
}

//
// Structs
//

//Path reservoir flag helper struct
struct ReservoirFlags {
    uint flags;

    __init() {
        flags = 0;
    }

    // Replaces path flag. Assumes that it is already or'ed
    [mutating]
    void setReservoirPathFlags(uint pathFlag) {
        flags &= 0xFFFFFF; // Clear all flag bits
        flags |= pathFlag;
    }

    property uint pathLength {
        get { return flags & 0xFF; }
        set {
            flags &= ~0xFF; // Clear lower bits
            flags |= (newValue & 0xFF);
        }
    }

    property uint rcPathLength {
        get { return (flags & 0xFF00) >> 8; }
        set {
            flags &= ~0xFF00; // Clear  bits
            flags |= (newValue & 0xFF) << 8;
        }
    }

    property uint confidence {
        get { return (flags & 0xFF0000) >> 16; }
        set {
            flags &= ~0xFF0000; // Clear  bits
            flags |= (newValue & 0xFF) << 16;
        }
    }

    property bool firstHitIsDelta {
        get { return (flags & (uint)ReservoirPathFlags::firstHitIsDelta) > 0; }
        set {
            flags &= ~(uint)ReservoirPathFlags::firstHitIsDelta; // Clear bit
            flags |= (newValue ? (uint)ReservoirPathFlags::firstHitIsDelta : 0);
        }
    }

    property bool isReconnectionSurface {
        get { return (flags & (uint)ReservoirPathFlags::isReconnectionSurface) > 0; }
        set {
            flags &= ~(uint)ReservoirPathFlags::isReconnectionSurface; // Clear bit
            flags |= (newValue ? (uint)ReservoirPathFlags::isReconnectionSurface : 0);
        }
    }

    property bool isEmissiveHit {
        get { return (flags & (uint)ReservoirPathFlags::isEmissiveHit) > 0; }
        set {
            flags &= ~(uint)ReservoirPathFlags::isEmissiveHit; // Clear bit
            flags |= (newValue ? (uint)ReservoirPathFlags::isEmissiveHit : 0);
        }
    }

    property bool isEnvMapHit {
        get { return (flags & (uint)ReservoirPathFlags::isEnvMapHit) > 0; }
        set {
            flags &= ~(uint)ReservoirPathFlags::isEnvMapHit; // Clear bit
            flags |= (newValue ? (uint)ReservoirPathFlags::isEnvMapHit : 0);
        }
    }

    property bool isNEE {
        get { return (flags & (uint)ReservoirPathFlags::isNEE) > 0; }
        set {
            flags &= ~(uint)ReservoirPathFlags::isNEE; // Clear bit
            flags |= (newValue ? (uint)ReservoirPathFlags::isNEE : 0);
        }
    }

    property bool reconLastHitSpecular {
        get { return (flags & (uint)ReservoirPathFlags::reconLastHitSpecular) > 0; }
        set {
            flags &= ~(uint)ReservoirPathFlags::reconLastHitSpecular; // Clear bit
            flags |= (newValue ? (uint)ReservoirPathFlags::reconLastHitSpecular : 0);
        }
    }

    property bool reconSpecular {
        get { return (flags & (uint)ReservoirPathFlags::reconSpecular) > 0; }
        set {
            flags &= ~(uint)ReservoirPathFlags::reconSpecular; // Clear bit
            flags |= (newValue ? (uint)ReservoirPathFlags::reconSpecular : 0);
        }
    }

    property bool isNeeAtRecon {
        get { return (flags & (uint)ReservoirPathFlags::isNeeAtRecon) > 0; }
        set {
            flags &= ~(uint)ReservoirPathFlags::isNeeAtRecon; // Clear bit
            flags |= (newValue ? (uint)ReservoirPathFlags::isNeeAtRecon : 0);
        }
    }
}

//Path Reservoir; Size: 96 Byte (24 floats)
struct Reservoir {
    SampleGenerator sgPathDirections; // Sample Generator for random direction (uint4)

    uint4 neeSgSeed;                  // Seed for Nee Replay

    PackedHitInfo reconnectionSurface;        // Packed reconnection surface hit

    float jacobian;            //(stored) jacobian determinant at the first hit
    ReservoirFlags flags;      // path length and flags
    float2 packedSampleWo;     // packed sample out direction

    float3 F;                  // Complete radiance of the path
    float weight;              // Unbiased contribution weight

    float3 rcThp;              // Path throughput after reconnection
    float jacobianOutgoing;    // Jacobian for the outgoing direction after reconnection

    __init() {
        sgPathDirections = SampleGenerator(uint2(0), 0);
        reconnectionSurface = {};
        neeSgSeed = uint4(0);
        jacobian = 1;
        packedSampleWo = float2(0);
        flags = ReservoirFlags();
        F = float3(0);
        c = 1;
        rcThp = float3(0);
        weight = 0.0;
        jacobianOutgoing = 1.0;
    }

    //Packed properties helpers

    property uint pathLength {
        get { return flags.pathLength; }
        set { flags.pathLength = newValue;}
    }

    //Path length of the reconnection hit
    property uint rcPathLength {
        get { return flags.rcPathLength; }
        set { flags.rcPathLength = newValue;}
    }

    property uint c {
        get { return flags.confidence; }
        set { flags.confidence = newValue;}
    }

    property float3 sampleWo {
        get { return oct_to_ndir_snorm(packedSampleWo);}
        set { packedSampleWo = ndir_to_oct_snorm(newValue);}
    }

    //Functions
    bool isValid() {
        return weight > 0 && jacobian > 0 && any(F > 0);
    }
}

// Surface for shifted hit before reconnection hit; Size: 24 Byte (6 floats)
struct ShiftedSurface
{
    PackedHitInfo packedHit;    //Surface of the shifted hit
    float2 packedWi;             //Packed in direction

    //Incoming direction helper
    property float3 Wi {
        get { return oct_to_ndir_snorm(packedWi);}
        set { packedWi = ndir_to_oct_snorm(newValue);}
    }

    __init()
    {
        packedHit = {};
        packedWi = float2(0);
    }
}

// Helper for packed path length
struct PackedPathLength
{
    uint data;

    property uint pathLength {
        get { return data & 0xFF; }
        set {
            data &= ~0xFF;
            data |= newValue & 0xFF;
        }
    }

    property uint diffuseBounces {
        get { return (data >> 8) & 0xFF; }
        set {
            data &= ~0xFF00;
            data |= (newValue & 0xFF) << 8;
        }
    }

    property uint specularBounces {
        get { return (data >> 16) & 0xFF; }
        set {
            data &= ~0xFF0000;
            data |= (newValue & 0xFF) << 16;
        }
    }

    property uint deltaBounces {
        get { return (data >> 24) & 0xFF; }
        set {
            data &= ~0xFF000000;
            data |= (newValue & 0xFF) << 24;
        }
    }

    __init() {
        data = 0;
    }

    // Compare path with max path length
    // Returns true if max path length was not reached
    bool comparePathLengts(PackedPathLength max) {
        bool valid = true;
        valid &= deltaBounces < max.deltaBounces;
        valid &= specularBounces < max.specularBounces;
        valid &= diffuseBounces < max.diffuseBounces;
        return valid;
    }
}

//Load in shading data from hit and direction
ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, rayDir, lod);
    
    return sd;
}

// Load in shading data from hit and a ray origin
ShadingData loadShadingDataFromOrigin(const HitInfo hit, const float3 rayOrigin, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    float3 toHit = normalize(v.posW - rayOrigin);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -toHit, lod);

    return sd;
}

//Linearizes 2D index
uint index2Dto1D(uint2 index, uint frameDimX) {
    return index.x + index.y * frameDimX;
}

//Reverse from above
uint2 index1Dto2D(uint index, uint frameDimX) {
    return uint2(index % frameDimX, index / frameDimX);
}

/* Evaluate MIS weight with balance heuristic
   \param[in] n0 Number of samples taken from the first sampling strategy.
    \param[in] p0 Pdf for the first sampling strategy.
    \param[in] n1 Number of samples taken from the second sampling strategy.
    \param[in] p1 Pdf for the second sampling strategy.
    \return Weight for the contribution from the first strategy (p0).
*/
float evalMIS(float n0, float p0, float n1, float p1)
{
    float q0 = n0 * p0;
    float q1 = n1 * p1;
    float sum = (q0 + q1);
    if (sum > 0)
        return q0 / (q0 + q1);
    else
        return 0;
}

bool dispatchShadowRay(ShadingData sd, float3 toLight, float distanceToLight, bool applyEpsilon = true) {
    // Create a DXR 1.1 query object to trace a ray (the <1> means use alpha testing)
    SceneRayQuery<1> rayQuery;
    float distance = applyEpsilon ? distanceToLight * kShadowRayEpsilon : distanceToLight ;
    const Ray ray = Ray(sd.computeNewRayOrigin(), toLight, 0.0, distance);
    return rayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff);
}
