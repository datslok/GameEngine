#include "engine/engine_commands.h"

#include "engine/debug_views.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace {
    // The fields a key names: the field itself, or every field in the group it names ("engine.render.bloom"), or all of
    // them for "all". A group's name must end at a dot, so "engine.render.bloo" names nothing.
    std::vector<SettingDescription> fieldsNamed(const std::vector<SettingDescription>& fields, const std::string& key) {
        if (key == "all") {
            return fields;
        }

        std::vector<SettingDescription> named;
        const std::string group = key + ".";

        for (const SettingDescription& field : fields) {
            if (field.key == key || field.key.compare(0, group.size(), group) == 0) {
                named.push_back(field);
            }
        }

        return named;
    }

    // What is said about a key nothing answers to: the nearest keys, and how to see them all.
    std::string unknownKeyText(const std::string& key, const std::vector<SettingDescription>& fields) {
        std::vector<std::string> keys;
        for (const SettingDescription& field : fields) {
            keys.push_back(field.key);
        }

        std::string text = "No setting or group called \"" + key + "\".";
        const std::vector<std::string> near = closestNames(key, keys);

        for (std::size_t index = 0; index < near.size(); ++index) {
            text += (index == 0 ? " Did you mean " : ", ") + near[index];
        }

        return text + (near.empty() ? "" : "?") + " \"list\" shows every setting.";
    }

    // The fields under get's or list's optional key, or all of them without one; std::invalid_argument when none match.
    std::vector<SettingDescription> fieldsToShow(const SettingsWalk& walk, const CommandArguments& arguments, const char* command) {
        if (arguments.size() > 1) {
            throw std::invalid_argument(std::string{command} + " takes at most one key or group");
        }

        const std::vector<SettingDescription> fields = describeSettings(walk);
        const std::vector<SettingDescription> named = arguments.empty() ? fields : fieldsNamed(fields, arguments.front());

        if (named.empty()) {
            throw std::invalid_argument(unknownKeyText(arguments.front(), fields));
        }

        return named;
    }

    CommandReply setCommand(const SettingsWalk& walk, const CommandArguments& arguments) {
        std::string key;
        std::string value;

        if (arguments.size() == 2) {
            key = arguments[0];
            value = arguments[1];
        } else if (arguments.size() == 1 && arguments[0].find('=') != std::string::npos) {
            const std::size_t equals = arguments[0].find('=');
            key = arguments[0].substr(0, equals);
            value = arguments[0].substr(equals + 1);
        } else {
            return commandError("Usage: set <key> <value>, or set <key>=<value>");
        }

        const SettingChange change = setSetting(walk, key, value);

        switch (change.outcome) {
            case SettingOutcome::Set:
                return commandOk(key + " = " + change.value);
            case SettingOutcome::Clamped:
                return commandOk(key + " = " + change.value + " (" + change.message + ")");
            case SettingOutcome::Refused:
                return commandError(change.message);
            case SettingOutcome::AtStartupOnly:
                return commandError(key + " is read only at start-up, so changing it now would do nothing. Give it at launch with --set " +
                                    key + "=" + value + ", or save it in " + savedSettingsPath + ".");
            case SettingOutcome::Unknown:
                break;
        }

        return commandError(unknownKeyText(key, describeSettings(walk)));
    }

    CommandReply resetCommand(const SettingsWalk& walk, const std::vector<SettingEntry>& defaults, const CommandArguments& arguments) {
        if (arguments.size() != 1) {
            return commandError("Usage: reset <key>, reset <group> or reset all");
        }

        const std::vector<SettingDescription> fields = describeSettings(walk);
        const std::vector<SettingDescription> named = fieldsNamed(fields, arguments.front());

        if (named.empty()) {
            return commandError(unknownKeyText(arguments.front(), fields));
        }

        std::vector<SettingEntry> entries;
        for (const SettingDescription& field : named) {
            const auto found = std::find_if(defaults.begin(), defaults.end(), [&](const SettingEntry& entry) {
                return entry.key == field.key;
            });

            if (!field.atStartup && found != defaults.end()) {
                entries.push_back(*found);
            }
        }

        applySettings(entries, walk, StartupFields::Keep);
        const std::size_t kept = named.size() - entries.size();
        return commandOk("Reset " + std::to_string(entries.size()) + " settings to their defaults" +
                         (kept > 0 ? " (" + std::to_string(kept) + " read only at start-up left as they are)" : ""));
    }

    // The camera's line, as the debug readout shows it.
    CommandReply cameraPose(const Camera& camera) {
        return commandOk(cameraText(camera.getPosition(), camera.getForward()));
    }
}

void addSettingsCommands(CommandRegistry& registry, SettingsWalk walk, std::vector<SettingEntry> defaults) {
    registry.add("set", "set <key> <value>",
                 "Changes a setting live, by its key in settings.cfg (\"list\" shows them), e.g. set engine.render.bloom.strength 0.1.",
                 [walk](const CommandArguments& arguments) {
                     return setCommand(walk, arguments);
                 });

    registry.add("get", "get [key or group]", "Shows a setting's value, or every value in a group (engine.render.bloom), or all.",
                 [walk](const CommandArguments& arguments) {
                     std::string text;
                     for (const SettingDescription& field : fieldsToShow(walk, arguments, "get")) {
                         text += (text.empty() ? "" : "\n") + field.key + " = " + field.value;
                     }
                     return commandOk(text);
                 });

    registry.add("list", "list [key or group]",
                 "Shows settings with what each takes (its kind, limits and unit) and whether it is read only at start-up.",
                 [walk](const CommandArguments& arguments) {
                     std::string text;
                     for (const SettingDescription& field : fieldsToShow(walk, arguments, "list")) {
                         text += (text.empty() ? "" : "\n") + field.key + " = " + field.value + "    (" + field.kind +
                                 (field.atStartup ? "; read only at start-up" : "") + ")";
                     }
                     return commandOk(text);
                 });

    registry.add("reset", "reset <key, group or all>", "Returns settings to their defaults (not saved until save-settings).",
                 [walk, defaults = std::move(defaults)](const CommandArguments& arguments) {
                     return resetCommand(walk, defaults, arguments);
                 });
}

int ticksThisFrame(const SimulationControl& control, int ticksDue) {
    return control.paused ? 0 : ticksDue;
}

void addSimulationCommands(CommandRegistry& registry, SimulationControl& control, std::function<void()> runTick) {
    registry.add("pause", "pause", "Stops the simulation's clock; frames still draw and the camera still moves.",
                 [&control](const CommandArguments& arguments) {
                     if (!arguments.empty()) {
                         return commandError("pause takes nothing");
                     }
                     control.paused = true;
                     return commandOk("Paused");
                 });

    registry.add("resume", "resume", "Starts the simulation's clock again after pause or step.", [&control](const CommandArguments& arguments) {
        if (!arguments.empty()) {
            return commandError("resume takes nothing");
        }
        control.paused = false;
        return commandOk("Running");
    });

    registry.add("step", "step [ticks]", "Pauses, then runs that many ticks (one without a count) at once, exactly as playing would.",
                 [&control, runTick = std::move(runTick)](const CommandArguments& arguments) {
                     if (arguments.size() > 1) {
                         return commandError("Usage: step [ticks]");
                     }

                     const int ticks = arguments.empty() ? 1 : intArgument(arguments.front(), "The ticks to step");

                     if (ticks < 1 || ticks > maxStepTicks) {
                         return commandError("step runs 1 to " + std::to_string(maxStepTicks) + " ticks, not " + std::to_string(ticks));
                     }

                     control.paused = true;
                     for (int tick = 0; tick < ticks; ++tick) {
                         runTick();
                     }

                     return commandOk("Stepped " + std::to_string(ticks) + " ticks (paused; resume to run)");
                 });
}

/*
* A place alone keeps the direction the camera looks; a target turns it there. The world's up is the camera's up, so
* looking straight up or down has no defined turn and is refused.
*/
void addCameraCommands(CommandRegistry& registry, Game& game, World& world) {
    registry.add("camera", "camera [x y z [tx ty tz]]",
                 "Shows the camera's place and direction, or places it at x y z (metres) looking at tx ty tz, or the same way as now.",
                 [&game, &world](const CommandArguments& arguments) {
                     Camera& camera = game.getCamera();

                     if (arguments.empty()) {
                         return cameraPose(camera);
                     }

                     if (arguments.size() != 3 && arguments.size() != 6) {
                         return commandError("Usage: camera, camera x y z, or camera x y z tx ty tz");
                     }

                     const Vec3 position{floatArgument(arguments[0], "x"), floatArgument(arguments[1], "y"), floatArgument(arguments[2], "z")};
                     const Vec3 target = arguments.size() == 6
                                             ? Vec3{floatArgument(arguments[3], "tx"), floatArgument(arguments[4], "ty"), floatArgument(arguments[5], "tz")}
                                             : position + camera.getForward();
                     const Vec3 direction = target - position;
                     const float horizontal = std::sqrt(direction.x * direction.x + direction.z * direction.z);

                     if (!(horizontal > 1e-4f * direction.length())) {
                         return commandError("The camera cannot look straight up or down, or at its own place");
                     }

                     if (!game.poseCamera(world, position, target)) {
                         return commandError(std::string{game.title()} + " cannot place its camera from commands");
                     }

                     return cameraPose(camera);
                 });
}
