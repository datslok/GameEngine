#include "render/gpu/light_priority.h"

#include "render/gpu/light_uniforms.h"
#include "render/gpu/shadow_map.h"

#include <algorithm>
#include <numeric>

// Every seated directional light can have a shadow, so only point lights and spotlights need a separate shadow choice.
static_assert(maxShadowedDirectionalLights >= maxDirectionalLights, "every seated directional light is expected to fit in the shadow atlas");

namespace {
    float brightestChannel(const Vec3& colour) {
        return std::max({colour.x, colour.y, colour.z});
    }

    // Entity{} belongs to no one, so a light without an entity never counts as last frame's.
    bool wasChosen(const Entity& entity, const std::vector<Entity>& incumbents) {
        return entity != Entity{} && std::find(incumbents.begin(), incumbents.end(), entity) != incumbents.end();
    }

    /*
    * Indices of up to count lights, most important first, with last frame's choices counting incumbentAdvantage times
    * as much. The sort is stable, so equally important lights keep the order they were collected in.
    */
    std::vector<std::size_t> chooseMostImportant(const std::vector<float>& importance, const std::vector<Entity>& entities,
                                                 const std::vector<Entity>& incumbents, int count) {
        std::vector<float> standing(importance.size());

        for (std::size_t i = 0; i < importance.size(); ++i) {
            standing[i] = wasChosen(entities[i], incumbents) ? importance[i] * incumbentAdvantage : importance[i];
        }

        std::vector<std::size_t> order(importance.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return standing[a] > standing[b];
        });

        if (order.size() > static_cast<std::size_t>(count)) {
            order.resize(static_cast<std::size_t>(count));
        }

        return order;
    }

    template <typename Placed>
    float importanceAtFocus(const Placed& placed, const Vec3& focus) {
        return lightImportance(placed.position, placed.light.colour, placed.light.intensity, placed.light.sourceRadius, focus);
    }

    /*
    * The seated lights of one kind, most important first, out of those the shader can draw and whose range sphere reaches
    * into view. Their entities are added to seatedEntities for next frame, and candidateCount says how many competed.
    */
    template <typename Placed>
    std::vector<Placed> chooseSeats(const std::vector<Placed>& lights, const Vec3& focus, const Frustum& view,
                                    const std::vector<Entity>& previouslySeated, int seats, std::vector<Entity>& seatedEntities,
                                    std::size_t& candidateCount) {
        std::vector<Placed> candidates;
        std::vector<float> importance;
        std::vector<Entity> entities;

        for (const Placed& placed : lights) {
            if (canBeDrawn(placed) && view.intersectsSphere(placed.position, placed.light.range)) {
                candidates.push_back(placed);
                importance.push_back(importanceAtFocus(placed, focus));
                entities.push_back(placed.entity);
            }
        }

        candidateCount = candidates.size();
        std::vector<Placed> seated;

        for (std::size_t index : chooseMostImportant(importance, entities, previouslySeated, seats)) {
            seated.push_back(candidates[index]);

            if (candidates[index].entity != Entity{}) {
                seatedEntities.push_back(candidates[index].entity);
            }
        }

        return seated;
    }

    /*
    * Of the seated lights that cast shadows, the most important keep castsShadows; the others are switched off for
    * this frame, so shadow planning gives tiles to exactly the chosen ones.
    */
    template <typename Placed>
    void chooseShadowedLights(std::vector<Placed>& seated, const Vec3& focus, const std::vector<Entity>& previouslyShadowed,
                              int shadows, std::vector<Entity>& shadowedEntities) {
        std::vector<std::size_t> casterSlots;
        std::vector<float> importance;
        std::vector<Entity> entities;

        for (std::size_t slot = 0; slot < seated.size(); ++slot) {
            if (seated[slot].light.castsShadows) {
                casterSlots.push_back(slot);
                importance.push_back(importanceAtFocus(seated[slot], focus));
                entities.push_back(seated[slot].entity);
            }
        }

        std::vector<bool> getsShadow(seated.size(), false);

        for (std::size_t caster : chooseMostImportant(importance, entities, previouslyShadowed, shadows)) {
            getsShadow[casterSlots[caster]] = true;

            if (entities[caster] != Entity{}) {
                shadowedEntities.push_back(entities[caster]);
            }
        }

        for (std::size_t slot = 0; slot < seated.size(); ++slot) {
            seated[slot].light.castsShadows = getsShadow[slot];
        }
    }
}

/*
* Like the shader, a source radius that is not positive counts as 1, which also keeps the division safe at the light itself.
*/
float lightImportance(const Vec3& lightPosition, const Vec3& colour, float intensity, float sourceRadius, const Vec3& focus) {
    const float radius = sourceRadius > 0.0f ? sourceRadius : 1.0f;
    const float distanceSquared = (lightPosition - focus).lengthSquared();
    return intensity * brightestChannel(colour) / (distanceSquared + radius * radius);
}

/*
* Directional lights light everything equally and have no position, so they are ranked by brightness alone and need no
* history: their brightness does not change as the camera moves.
*/
PrioritizedLighting prioritizeLights(const FrameLighting& lighting, const Vec3& focus, const Frustum& view, const LightHistory& previous) {
    PrioritizedLighting result;
    result.lighting.ambient = lighting.ambient;

    for (const DirectionalLight& light : lighting.directionalLights) {
        if (canBeDrawn(light)) {
            result.lighting.directionalLights.push_back(light);
        }
    }

    std::vector<DirectionalLight>& directional = result.lighting.directionalLights;
    std::stable_sort(directional.begin(), directional.end(), [](const DirectionalLight& a, const DirectionalLight& b) {
        return a.intensity * brightestChannel(a.colour) > b.intensity * brightestChannel(b.colour);
    });

    if (directional.size() > static_cast<std::size_t>(maxDirectionalLights)) {
        directional.resize(static_cast<std::size_t>(maxDirectionalLights));
    }

    result.lighting.pointLights = chooseSeats(lighting.pointLights, focus, view, previous.litPoints, maxPointLights,
                                              result.history.litPoints, result.pointLightsInView);
    chooseShadowedLights(result.lighting.pointLights, focus, previous.shadowedPoints, maxShadowedPointLights,
                         result.history.shadowedPoints);

    std::size_t spotLightsInView = 0;
    result.lighting.spotLights = chooseSeats(lighting.spotLights, focus, view, previous.litSpots, maxSpotLights,
                                             result.history.litSpots, spotLightsInView);
    chooseShadowedLights(result.lighting.spotLights, focus, previous.shadowedSpots, maxShadowedSpotLights,
                         result.history.shadowedSpots);

    return result;
}
