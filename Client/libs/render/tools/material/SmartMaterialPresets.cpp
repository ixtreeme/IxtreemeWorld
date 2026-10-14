#include "MaterialGraphDocument.h"

namespace material_tool
{
MaterialGraphDocument MaterialGraphDocument::WeatheredStone()
{
    MaterialGraphDocument d; std::string error;
    const auto stone = d.AddMaterialLayer("Stone Base", error);
    const auto dirt = d.AddMaterialLayer("Dirt Layer", error);
    const auto moss = d.AddMaterialLayer("Moss Layer", error);
    const auto connect = [&](std::uint32_t from, std::size_t output, std::uint32_t to, std::size_t input) {
        d.Connect(PinId(from, output), PinId(to, input), error, true);
    };
    const auto parameter = [&](const char* name, float value, std::array<float, 2> at) {
        const auto id = d.AddNode(NodeKind::ScalarParameter, at);
        auto& node = *d.FindNode(id); node.parameter = name; node.name = name; node.value[0] = value; return id;
    };
    const auto texture = [&](const char* name, std::array<float,4> fallback, bool srgb, std::array<float,2> at) {
        const auto input = d.AddNode(NodeKind::TextureParameter, at), sample = d.AddNode(NodeKind::TextureSample, {at[0]+300,at[1]});
        auto& node = *d.FindNode(input); node.parameter = name; node.name = name; node.value = fallback;
        d.FindNode(sample)->srgb = srgb; connect(input,0,sample,0); return sample;
    };
    const auto source = [&](std::uint32_t id) { return d.FindLayer(id)->sourceNode; };
    d.FindNode(source(stone))->settings[0] = .8f;
    d.FindNode(source(dirt))->value = {.12f,.075f,.035f,1}; d.FindNode(source(dirt))->settings[0] = .95f;
    d.FindNode(source(moss))->value = {.055f,.13f,.025f,1}; d.FindNode(source(moss))->settings[0] = .88f;

    const auto tiling = d.AddNode(NodeKind::Vector2Parameter, {-1200,-600});
    d.FindNode(tiling)->parameter = "StoneTiling"; d.FindNode(tiling)->name = "Stone Tiling"; d.FindNode(tiling)->value = {2,2,0,0};
    const auto uv = d.AddNode(NodeKind::TilingOffset, {-800,-600}); connect(tiling,0,uv,1);
    const auto albedo = texture("StoneBaseColor", {.6f,.58f,.56f,1}, true, {-1200,-300});
    const auto normal = texture("StoneNormal", {.5f,.5f,1,1}, false, {-1200,0});
    const auto height = texture("StoneHeight", {0,0,0,1}, false, {-1200,300});
    connect(uv,3,albedo,1); connect(uv,3,normal,1); connect(uv,3,height,1);
    connect(albedo,2,source(stone),0); connect(height,3,source(stone),6);
    const auto strength = parameter("StoneNormalStrength",1,{-800,300});
    const auto normalStrength = d.AddNode(NodeKind::NormalStrength, {-400,0});
    connect(normal,7,normalStrength,0); connect(strength,0,normalStrength,1); connect(normalStrength,2,source(stone),4);

    const auto cavity = d.AddNode(NodeKind::Cavity, {-1200,600}), ao = d.AddNode(NodeKind::AO, {-1200,800});
    const auto occlusion = d.AddNode(NodeKind::MaskInvert, {-900,800}); connect(ao,0,occlusion,0);
    const auto recess = d.AddNode(NodeKind::MaskCombine, {-600,600}); d.FindNode(recess)->settings[0]=1;
    connect(cavity,0,recess,0); connect(occlusion,1,recess,1);
    const auto dirtNoise = d.AddNode(NodeKind::Noise, {-900,1100}); d.FindNode(dirtNoise)->value={2,4,.55f,19}; d.FindNode(dirtNoise)->settings[0]=1;
    const auto dirtNoiseMask = d.AddNode(NodeKind::MaskCombine, {-300,700}); connect(recess,2,dirtNoiseMask,0); connect(dirtNoise,2,dirtNoiseMask,1);
    const auto dirtContrast = parameter("DirtContrast",1,{-300,1100});
    const auto contrast = d.AddNode(NodeKind::MaskContrast, {0,700}); connect(dirtNoiseMask,2,contrast,0); connect(dirtContrast,0,contrast,1);
    const auto dirtAmount = parameter("DirtAmount",1,{0,1100});
    const auto dirtMask = d.AddNode(NodeKind::Multiply, {300,900}); connect(contrast,2,dirtMask,0); connect(dirtAmount,0,dirtMask,1);
    d.SetMaterialLayerMask(dirt, PinId(dirtMask,2), error);

    const auto slope = d.AddNode(NodeKind::SlopeMask, {-1200,1500}); d.FindNode(slope)->value={25,65,0,0};
    const auto threshold = parameter("MossSlopeThreshold",25,{-1500,1750}); connect(threshold,0,slope,1);
    const auto up = d.AddNode(NodeKind::MaskInvert, {-900,1500}); connect(slope,3,up,0);
    const auto mossScale = parameter("MossScale",2,{-1500,2050}), mossSeed = parameter("MossSeed",23,{-1500,2300});
    const auto noise = d.AddNode(NodeKind::Noise, {-1200,2050}); d.FindNode(noise)->value={2,4,.55f,23}; d.FindNode(noise)->settings[0]=1;
    connect(mossScale,0,noise,1); connect(mossSeed,0,noise,3);
    const auto growth = d.AddNode(NodeKind::MaskCombine, {-600,1550}); connect(up,1,growth,0); connect(noise,2,growth,1);
    const auto cavityGrowth = d.AddNode(NodeKind::Multiply, {-600,2000}); d.FindNode(cavityGrowth)->value[1]=.15f; connect(cavity,0,cavityGrowth,0);
    const auto coverage = d.AddNode(NodeKind::MaskCombine, {-300,1500}); d.FindNode(coverage)->settings[0]=1;
    connect(growth,2,coverage,0); connect(cavityGrowth,2,coverage,1);
    const auto amount = parameter("MossAmount",1,{-300,1900});
    const auto mask = d.AddNode(NodeKind::Multiply, {0,1500}); connect(coverage,2,mask,0); connect(amount,0,mask,1);
    d.SetMaterialLayerMask(moss,PinId(mask,2),error);
    const auto mossNormal = texture("MossNormal",{.5f,.5f,1,1},false,{-1200,2600}); connect(mossNormal,7,source(moss),4);
    return d;
}
// A transparent water surface for a static mesh (a pond, a pool, a river plane), drawn by its live
// graph shader: waves of two drifting noise layers, colour and clarity by the depth of water behind
// the surface, foam along the shore and on wave crests, and the sky mirrored at grazing angles.
MaterialGraphDocument MaterialGraphDocument::Water()
{
    MaterialGraphDocument d; std::string error;
    const auto node = [&](NodeKind kind, std::array<float, 2> at, const char* name = nullptr) {
        const auto id = d.AddNode(kind, at); if (name) d.FindNode(id)->name = name; return id;
    };
    const auto connect = [&](std::uint32_t from, std::size_t output, std::uint32_t to, std::size_t input) {
        d.Connect(PinId(from, output), PinId(to, input), error);
    };
    const auto parameter = [&](const char* name, const char* label, float value, std::array<float, 2> at) {
        const auto id = node(NodeKind::ScalarParameter, at, label); auto& n = *d.FindNode(id); n.parameter = name; n.value[0] = value; return id;
    };
    const auto color = [&](const char* name, const char* label, std::array<float, 4> value, std::array<float, 2> at) {
        const auto id = node(NodeKind::ColorParameter, at, label); auto& n = *d.FindNode(id); n.parameter = name; n.value = value; return id;
    };
    // Scalar math with a constant second operand.
    const auto math = [&](NodeKind kind, std::uint32_t from, std::size_t output, float constant, std::array<float, 2> at) {
        const auto id = node(kind, at); d.FindNode(id)->value[1] = constant; connect(from, output, id, 0); return id;
    };
    const auto smooth = [&](std::uint32_t from, std::size_t output, float low, float high, std::array<float, 2> at) {
        const auto id = node(NodeKind::Smoothstep, at); auto& n = *d.FindNode(id); n.value[1] = low; n.value[2] = high; connect(from, output, id, 0); return id;
    };

    // Waves: two noise layers over the world XZ plane, drifting with time, as tangent-space normals.
    const auto position = node(NodeKind::WorldPosition, {-1700, 0});
    const auto split = node(NodeKind::SplitVector, {-1450, 0}); connect(position, 0, split, 0);
    const auto time = node(NodeKind::Time, {-1700, 250});
    const auto speed = parameter("WaveSpeed", "Wave speed (m/s)", .4f, {-1700, 400});
    const auto drift = node(NodeKind::Multiply, {-1450, 300}); connect(time, 0, drift, 0); connect(speed, 0, drift, 1);
    const auto scale = parameter("WaveScale", "Wave scale (1/m)", .35f, {-1700, 550});
    const auto xA = node(NodeKind::Add, {-1200, -100}); connect(split, 1, xA, 0); connect(drift, 2, xA, 1);
    const auto evolve = math(NodeKind::Multiply, drift, 2, .6f, {-1200, 300});
    const auto pA = node(NodeKind::MakeVector, {-950, -100}); connect(xA, 2, pA, 0); connect(split, 3, pA, 1); connect(evolve, 2, pA, 2);
    const auto noiseA = node(NodeKind::Noise, {-700, -100}, "Broad waves");
    d.FindNode(noiseA)->value = {1, 4, .5f, 3}; d.FindNode(noiseA)->settings[1] = 1.5f;
    connect(pA, 3, noiseA, 0); connect(scale, 0, noiseA, 1);
    const auto driftB = math(NodeKind::Multiply, drift, 2, .8f, {-1200, 150});
    const auto zB = node(NodeKind::Subtract, {-950, 150}); connect(split, 3, zB, 0); connect(driftB, 2, zB, 1);
    const auto pB = node(NodeKind::MakeVector, {-700, 150}); connect(split, 1, pB, 0); connect(zB, 2, pB, 1); connect(evolve, 2, pB, 2);
    const auto scaleB = math(NodeKind::Multiply, scale, 0, 2.7f, {-950, 450});
    const auto noiseB = node(NodeKind::Noise, {-450, 150}, "Ripples");
    d.FindNode(noiseB)->value = {1, 3, .5f, 11}; d.FindNode(noiseB)->settings[1] = 1.5f;
    connect(pB, 3, noiseB, 0); connect(scaleB, 2, noiseB, 1);
    const auto waves = node(NodeKind::NormalBlend, {-200, 0}); d.FindNode(waves)->value[0] = .7f;
    connect(noiseA, 4, waves, 0); connect(noiseB, 4, waves, 1);
    const auto waveHeight = parameter("WaveHeight", "Wave height", .5f, {-450, 400});
    // Seen at a grazing angle the ripples lie flatter: their full tilt would flip the reflection on
    // every crest and sparkle in the distance.
    const auto grazing = node(NodeKind::Fresnel, {-450, 550}, "Grazing view"); d.FindNode(grazing)->value[0] = 1;
    const auto grazingRamp = smooth(grazing, 3, .7f, .97f, {-200, 550});
    const auto flatten = node(NodeKind::Lerp, {50, 550}); d.FindNode(flatten)->value = {1, .3f, 0, 0}; connect(grazingRamp, 3, flatten, 2);
    const auto tilt = node(NodeKind::Multiply, {50, 400}); connect(waveHeight, 0, tilt, 0); connect(flatten, 3, tilt, 1);
    const auto normal = node(NodeKind::NormalStrength, {300, -150}); connect(waves, 3, normal, 0); connect(tilt, 2, normal, 1);

    // Depth: the water behind the surface sets its colour and how much of the bottom shows through.
    const auto depth = node(NodeKind::SceneDepth, {-1700, 800}); d.FindNode(depth)->value[0] = 3;
    const auto range = parameter("DepthRange", "Depth range (m)", 4, {-1700, 950});
    const auto depthRatio = node(NodeKind::Divide, {-1450, 850}); connect(depth, 0, depthRatio, 0); connect(range, 0, depthRatio, 1);
    const auto deepness = node(NodeKind::Saturate, {-1200, 850}); connect(depthRatio, 2, deepness, 0);
    const auto shallow = color("ShallowColor", "Shallow colour", {.06f, .32f, .30f, 1}, {-1200, 1000});
    const auto deep = color("DeepColor", "Deep colour", {.005f, .04f, .08f, 1}, {-1200, 1150});
    const auto water = node(NodeKind::Blend, {-950, 1000}); connect(shallow, 0, water, 0); connect(deep, 0, water, 1); connect(deepness, 1, water, 2);
    const auto clarity = node(NodeKind::Lerp, {-950, 850}); d.FindNode(clarity)->value = {.35f, .95f, 0, 0}; connect(deepness, 1, clarity, 2);

    // Foam along the shore (little water behind the surface), broken up by the ripples, and on crests.
    const auto foamDistance = parameter("FoamDistance", "Shore foam width (m)", .5f, {-1700, 1300});
    const auto shoreRamp = node(NodeKind::Smoothstep, {-1450, 1300}); d.FindNode(shoreRamp)->value[1] = 0;
    connect(depth, 0, shoreRamp, 0); connect(foamDistance, 0, shoreRamp, 2);
    const auto shore = node(NodeKind::OneMinus, {-1200, 1300}); connect(shoreRamp, 3, shore, 0);
    const auto breakup = smooth(noiseB, 2, .35f, .6f, {-1200, 1450});
    const auto shoreFoam = node(NodeKind::Multiply, {-950, 1350}); connect(shore, 1, shoreFoam, 0); connect(breakup, 3, shoreFoam, 1);
    const auto crest = smooth(noiseA, 2, .74f, .82f, {-950, 1500});
    const auto crestFoam = math(NodeKind::Multiply, crest, 3, .35f, {-700, 1500});
    const auto foamSum = node(NodeKind::Add, {-700, 1350}); connect(shoreFoam, 2, foamSum, 0); connect(crestFoam, 2, foamSum, 1);
    const auto foamAmount = parameter("FoamAmount", "Foam amount", 1, {-950, 1650});
    const auto foamScaled = node(NodeKind::Multiply, {-450, 1350}); connect(foamSum, 2, foamScaled, 0); connect(foamAmount, 0, foamScaled, 1);
    const auto foam = node(NodeKind::Saturate, {-200, 1350}, "Foam"); connect(foamScaled, 2, foam, 0);
    const auto foamColor = node(NodeKind::Color, {-200, 1500}, "Foam colour"); d.FindNode(foamColor)->value = {.85f, .88f, .9f, 1};

    // Surface: water or foam, glossy water, opaque foam.
    const auto base = node(NodeKind::Blend, {300, 900}); connect(water, 3, base, 0); connect(foamColor, 0, base, 1); connect(foam, 1, base, 2);
    const auto roughness = node(NodeKind::Lerp, {300, 1100}); d.FindNode(roughness)->value = {.03f, .55f, 0, 0}; connect(foam, 1, roughness, 2);
    const auto opacity = node(NodeKind::Maximum, {300, 1250}); connect(clarity, 3, opacity, 0); connect(foam, 1, opacity, 1);

    // The sky mirrored by the waves, strongest at grazing angles, none on foam.
    const auto sky = node(NodeKind::SkyReflection, {300, 0}); connect(normal, 2, sky, 0);
    const auto fresnel = node(NodeKind::Fresnel, {300, 200});
    const auto reflection = parameter("ReflectionStrength", "Reflection strength", 1, {300, 400});
    const auto mirrored = node(NodeKind::Multiply, {550, 250}); connect(fresnel, 3, mirrored, 0); connect(reflection, 0, mirrored, 1);
    const auto clear = node(NodeKind::OneMinus, {550, 450}); connect(foam, 1, clear, 0);
    const auto weight = node(NodeKind::Multiply, {800, 300}); connect(mirrored, 2, weight, 0); connect(clear, 1, weight, 1);
    const auto emissive = node(NodeKind::Blend, {800, 0}, "Sky reflection"); d.FindNode(emissive)->value = {0, 0, 0, 1};
    connect(sky, 1, emissive, 1); connect(weight, 2, emissive, 2);

    const auto output = node(NodeKind::PbrOutput, {1100, 500});
    connect(base, 3, output, 0); connect(roughness, 3, output, 1); connect(emissive, 3, output, 3);
    connect(normal, 2, output, 4); connect(opacity, 2, output, 7);
    return d;
}
}
