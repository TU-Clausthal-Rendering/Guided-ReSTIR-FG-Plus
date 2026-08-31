#pragma once
#include "Utils/Math/MathConstants.slangh"

import Scene.RaytracingInline;
import Rendering.Lights.LightHelpers;
import Utils.Sampling.SampleGenerator;
import Rendering.Lights.EmissiveLightSampler;
import Rendering.Lights.EmissiveLightSamplerHelpers;
import Rendering.Lights.EnvMapSampler;
import StructsAndHelpers;

cbuffer Light {
    EmissiveLightSampler gEmissiveSampler;
    EnvMapSampler gEnvMapSampler;
}

static const uint kNeeLightTypeEmissive = 0;
static const uint kNeeLightTypeAnalytic = 1;
static const uint kNeeLightTypeEnvMap = 2;

// Nee light sample
struct LightSample {
    uint type;
    float3 Li;
    float pdf;
    float3 position;
    float3 dir;
    float distance;
    float3 faceN;
    bool isInfiniteDistance;
}

bool generateEmissiveSample(ShadingData sd, inout SampleGenerator sg, inout LightSample ls)
{
    TriangleLightSample tls = {};
    if (!gEmissiveSampler.sampleLight(sd.posW, sd.faceN, true, sg, tls))
        return false;

    ls.pdf *= tls.pdf;
    ls.Li = ls.pdf > 0.f ? tls.Le / ls.pdf : float3(0);
    ls.position = tls.posW;
    ls.faceN = tls.normalW;
    ls.distance = tls.distance;
    ls.dir = tls.dir;
    ls.isInfiniteDistance = false;

    return any(ls.Li > 0);
}

bool generateAnalyticSample(ShadingData sd, inout SampleGenerator sg, inout LightSample ls) {
    uint lightCount = gScene.getLightCount();
    if (lightCount == 0) return false;

    uint lightIndex = min(uint(sampleNext1D(sg) * lightCount), lightCount - 1);
    // Sample local light source.
    AnalyticLightSample analyticSample;
    if (!sampleLight(sd.posW, gScene.getLight(lightIndex), sg, analyticSample)) return false;

    ls.Li = ls.pdf > 0 ? (analyticSample.Li * lightCount) / ls.pdf : float3(0); // analyticSample.Li is already divided by the pdf
    ls.isInfiniteDistance = ls.distance == kMaxLightDistance;
    ls.pdf /= lightCount;
    ls.distance = analyticSample.distance;
    ls.dir = analyticSample.dir;
    ls.faceN = ls.dir;
    ls.position = analyticSample.posW;

    return any(ls.Li > 0);
}

bool generateEnvMapSample(ShadingData sd, inout SampleGenerator sg, inout LightSample ls) {
    // Sample environment map.
    EnvMapSample envSample;
    if (!gEnvMapSampler.sample(sampleNext2D(sg), envSample)) return false;

    ls.pdf *= envSample.pdf;
    ls.Li = ls.pdf > 0.f ? envSample.Le / ls.pdf : float3(0);
    ls.distance = FLT_MAX;
    ls.dir = envSample.dir;
    ls.isInfiniteDistance = true;

    return any(ls.Li > 0);
}

bool selectLightType(out uint lightType, out float pdf, float u, float3 lightTypeSelectProbability)
{
    float3 p = lightTypeSelectProbability;
    //[unroll]
    for (lightType = 0; lightType < 3; ++lightType)
    {
        if (u < p[lightType])
        {
            pdf = p[lightType];
            return true;
        }
        u -= p[lightType];
    }

    lightType = {};
    pdf = {};

    return false;
}

bool generateAndSelectLightSample(ShadingData sd, float3 lightTypeSelectProbability, inout LightSample ls, inout SampleGenerator sg)
{
    if (!selectLightType(ls.type, ls.pdf, sampleNext1D(sg), lightTypeSelectProbability))
        return false;

    bool valid = false;
    if (ls.type == kNeeLightTypeEmissive)
        valid = generateEmissiveSample(sd, sg, ls);
    if (ls.type == kNeeLightTypeAnalytic)
        valid = generateAnalyticSample(sd, sg, ls);
    if (ls.type == kNeeLightTypeEnvMap)
        valid = generateEnvMapSample(sd, sg, ls);

    return valid;
}

float3 nextEventEstimation(ShadingData sd, IMaterialInstance mi, float3 lightTypeSelectProbability, out LightSample ls, inout SampleGenerator sg)
{
    ls = {};
    if(!generateAndSelectLightSample(sd, lightTypeSelectProbability, ls, sg))
        return float3(0);

    float3 bsdf = mi.eval(sd, ls.dir, sg);

    if (all(bsdf <= 0) || ls.pdf <= 0)
        return float3(0);

    // Shadow Ray
    if (!dispatchShadowRay(sd, ls.dir, ls.distance))
        return float3(0);
    
    
    float misWeight = 1.0;
    if (ls.type != kNeeLightTypeAnalytic) {
        float scatterPdf = mi.evalPdf(sd, ls.dir);
        misWeight = evalMIS(1.0, ls.pdf, 1.0, scatterPdf);
    }

    return misWeight * bsdf * ls.Li;
}
