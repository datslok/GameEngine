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

    // What decides one light's place in a ranking.
    struct Contender {
        int priority;
        float importance;
        Entity entity;
    };

    template <typename Placed>
    Contender contenderFor(const Placed& placed, const Vec3& focus) {
        return Contender{
            placed.light.priority,
            lightImportance(placed.position, placed.light.colour, placed.light.intensity, placed.light.sourceRadius, focus),
            placed.entity
        };
    }

    /*
    * Indices of up to count lights, most important first. Priority comes first: a higher one always wins. Between equal
    * priorities, last frame's choices count incumbentAdvantage times as much. The sort is stable, so equally important
    * lights keep the order they were collected in.
    */
    std::vector<std::size_t> chooseMostImportant(const std::vector<Contender>& contenders, const std::vector<Entity>& incumbents, int count) {
        std::vector<float> standing(contenders.size());

        for (std::size_t i = 0; i < contenders.size(); ++i) {
            const Contender& contender = contenders[i];
            standing[i] = wasChosen(contender.entity, incumbents) ? contender.importance * incumbentAdvantage : contender.importance;
        }

        std::vector<std::size_t> order(contenders.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            if (contenders[a].priority != contenders[b].priority) {
                return contenders[a].priority > contenders[b].priority;
            }

            return standing[a] > standing[b];
        });

        if (order.size() > static_cast<std::size_t>(count)) {
            order.resize(static_cast<std::size_t>(count));
        }

        return order;
    }


    // A fade that starts this frame begins at least this far in, so a holder always shows as one in the history, even
    // in a frame that took no time.
    constexpr float smallestFade = 0.001f;

    // Entity{} belongs to no one, so a light without an entity is never found: it is new every frame.
    const LightFade* findHeld(const std::vector<LightFade>& held, const Entity& entity) {
        if (entity == Entity{}) {
            return nullptr;
        }

        for (const LightFade& fade : held) {
            if (fade.entity == entity) {
                return &fade;
            }
        }

        return nullptr;
    }

    bool wasInView(const LightHistory& previous, const Entity& entity) {
        return entity != Entity{} && std::find(previous.inView.begin(), previous.inView.end(), entity) != previous.inView.end();
    }

    std::vector<Entity> holdersOf(const std::vector<LightFade>& held, bool shadowsOnly) {
        std::vector<Entity> entities;

        for (const LightFade& fade : held) {
            if (!shadowsOnly || fade.shadow > 0.0f) {
                entities.push_back(fade.entity);
            }
        }

        return entities;
    }

    // A light holding a seat this frame: which candidate, how far into its seat and its shadow, and how bright it was last frame.
    struct Holder {
        std::size_t candidate;
        float light;
        float shadow;
        float lightLastFrame;
        bool holdsShadow;
    };

    /*
    * Seats for one kind of light, out of those the shader can draw and whose range sphere reaches into view.
    * - The most important (priority, then brightness, with last frame's holders favoured) are wanted.
    * - Last frame's holders keep their seat: a wanted one fades in (or stays full), an unwanted one fades out and gives
    *   the seat up only when it reaches zero.
    * - Wanted newcomers take the seats left, most important first: they fade in if they were on screen last frame
    *   (unlit, waiting), and start full if they were not (nothing showed them, so nothing can pop).
    * Lights that left the view are simply not candidates, so their seats are free at once.
    */
    template <typename Placed>
    std::vector<Holder> holdSeats(const std::vector<Placed>& candidates, const std::vector<Contender>& contenders,
                                  const LightHistory& previous, const std::vector<LightFade>& previouslyHeld, int seats, float fadeStep) {
        const std::vector<std::size_t> wantedOrder = chooseMostImportant(contenders, holdersOf(previouslyHeld, false), seats);
        std::vector<bool> wanted(candidates.size(), false);

        for (std::size_t index : wantedOrder) {
            wanted[index] = true;
        }

        std::vector<Holder> holders;

        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (const LightFade* before = findHeld(previouslyHeld, candidates[i].entity)) {
                const float light = wanted[i] ? std::min(1.0f, before->light + fadeStep) : before->light - fadeStep;

                if (wanted[i] || light > 0.0f) {
                    holders.push_back(Holder{i, std::max(light, 0.0f), before->shadow, before->light, false});
                }
            }
        }

        for (std::size_t index : wantedOrder) {
            if (static_cast<int>(holders.size()) >= seats) {
                break;
            }

            if (findHeld(previouslyHeld, candidates[index].entity) != nullptr) {
                continue;
            }

            const float light = wasInView(previous, candidates[index].entity) ? std::clamp(fadeStep, smallestFade, 1.0f) : 1.0f;
            holders.push_back(Holder{index, light, 0.0f, 0.0f, false});
        }

        return holders;
    }

    /*
    * Shadows among the seated lights that cast them, handed over the same way: last frame's shadow holders fade in if
    * still wanted and out if not, keeping their tile until they reach zero; wanted newcomers take the tiles left. A new
    * shadow on a light that was already shining fades in; on a light that was not, it starts full, since the light itself
    * is only now appearing.
    */
    template <typename Placed>
    void holdShadows(std::vector<Holder>& holders, const std::vector<Placed>& candidates, const Vec3& focus,
                     const std::vector<LightFade>& previouslyHeld, int shadows, float fadeStep) {
        std::vector<std::size_t> casters;
        std::vector<Contender> contenders;

        for (std::size_t h = 0; h < holders.size(); ++h) {
            if (candidates[holders[h].candidate].light.castsShadows) {
                casters.push_back(h);
                contenders.push_back(contenderFor(candidates[holders[h].candidate], focus));
            }
            else {
                holders[h].shadow = 0.0f;
            }
        }

        const std::vector<std::size_t> wantedOrder = chooseMostImportant(contenders, holdersOf(previouslyHeld, true), shadows);
        std::vector<bool> wanted(casters.size(), false);

        for (std::size_t index : wantedOrder) {
            wanted[index] = true;
        }

        int held = 0;

        for (std::size_t c = 0; c < casters.size(); ++c) {
            Holder& holder = holders[casters[c]];

            if (holder.shadow <= 0.0f) {
                continue;
            }

            const float shadow = wanted[c] ? std::min(1.0f, holder.shadow + fadeStep) : holder.shadow - fadeStep;
            holder.holdsShadow = wanted[c] || shadow > 0.0f;
            holder.shadow = holder.holdsShadow ? std::max(shadow, smallestFade) : 0.0f;
            held += holder.holdsShadow ? 1 : 0;
        }

        for (std::size_t c : wantedOrder) {
            Holder& holder = holders[casters[c]];

            if (held >= shadows) {
                break;
            }

            if (holder.holdsShadow) {
                continue;
            }

            holder.holdsShadow = true;
            holder.shadow = holder.lightLastFrame > 0.0f ? std::clamp(fadeStep, smallestFade, 1.0f) : 1.0f;
            ++held;
        }
    }

    /*
    * The seated lights of one kind with their fades, and their entries for next frame's history. candidateCount says
    * how many competed.
    */
    template <typename Placed>
    std::vector<Placed> chooseLights(const std::vector<Placed>& lights, const Vec3& focus, const Frustum& view,
                                     const LightHistory& previous, const std::vector<LightFade>& previouslyHeld,
                                     int seats, int shadows, float fadeStep, LightHistory& history,
                                     std::vector<LightFade>& held, std::size_t& candidateCount) {
        std::vector<Placed> candidates;
        std::vector<Contender> contenders;

        for (const Placed& placed : lights) {
            if (canBeDrawn(placed) && view.intersectsSphere(placed.position, placed.light.range)) {
                candidates.push_back(placed);
                contenders.push_back(contenderFor(placed, focus));

                if (placed.entity != Entity{}) {
                    history.inView.push_back(placed.entity);
                }
            }
        }

        candidateCount = candidates.size();

        std::vector<Holder> holders = holdSeats(candidates, contenders, previous, previouslyHeld, seats, fadeStep);
        holdShadows(holders, candidates, focus, previouslyHeld, shadows, fadeStep);

        std::vector<Placed> seated;

        for (const Holder& holder : holders) {
            Placed placed = candidates[holder.candidate];
            placed.fade = holder.light;
            placed.shadowFade = holder.holdsShadow ? holder.shadow : 0.0f;
            placed.light.castsShadows = holder.holdsShadow;
            seated.push_back(placed);

            if (placed.entity != Entity{}) {
                held.push_back(LightFade{placed.entity, std::max(holder.light, smallestFade), placed.shadowFade});
            }
        }

        return seated;
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
PrioritizedLighting prioritizeLights(const FrameLighting& lighting, const Vec3& focus, const Frustum& view, const LightHistory& previous,
                                     float elapsedSeconds, const LightLimits& limits) {
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

    const float fadeStep = std::max(elapsedSeconds, 0.0f) / lightFadeSeconds;

    result.lighting.pointLights = chooseLights(lighting.pointLights, focus, view, previous, previous.points, limits.pointSeats,
                                               limits.shadowedPoints, fadeStep, result.history, result.history.points,
                                               result.pointLightsInView);

    std::size_t spotLightsInView = 0;
    result.lighting.spotLights = chooseLights(lighting.spotLights, focus, view, previous, previous.spots, limits.spotSeats,
                                              limits.shadowedSpots, fadeStep, result.history, result.history.spots,
                                              spotLightsInView);

    return result;
}
