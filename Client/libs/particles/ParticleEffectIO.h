#pragma once

// The .particle effect file format: one reusable Particle System parameter preset. The editor
// writes it (AssetLibrary::CreateParticleEffect) and applies it to a component (the inspector's
// preset picker copies the parameters into the entity's component, so a packaged game needs no
// asset access at runtime). JSON, parsed with the engine's string-scan helpers like prefabs.

#include "ParticleComponents.h"

#include <string>

namespace ixparticle
{

// The full effect parameter set as a JSON document (pretty-printed, UTF-8). `id`/`displayName`
// identify the asset for the library (the parser ignores them).
std::string WriteParticleEffectJson(const ParticleSystemComponent& effect,
                                    const std::string& id,
                                    const std::string& displayName);

// Parses a .particle document. Returns false when the text is not a JSON object. Missing fields
// keep their default values.
bool ParseParticleEffectJson(const std::string& text, ParticleSystemComponent& out);

} // namespace ixparticle
