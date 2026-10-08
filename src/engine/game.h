#pragma once

#include "ecs/world.h"
#include "input/input.h"
#include "scene/camera.h"

/*
* The boundary between the engine and one particular game.
* The engine owns the loop, the window, the renderer and the World, and calls these hooks at fixed points in each frame. A game fills the World, decides what input means, and runs its systems.
* A Game only sees World, Input and Camera, never SDL or the GPU, so it can also run headless (tests, a dedicated server).
*
* Each frame the engine calls, in order:
*   onInput        once, before the ticks: turn input into commands
*   onFixedUpdate  zero or more times: one fixed simulation tick each
*   onUpdate       once, after the ticks: camera and anything that follows interpolated positions
* then draws the World from getCamera().
*/
class Game {
public:
    virtual ~Game() = default;

    // Called once before the first frame: create entities and load assets.
    virtual void onInit(World& world) = 0;

    // Read this frame's input and turn it into commands for the simulation.
    virtual void onInput(World& world, const Input& input) = 0;

    // Advance the game by exactly one tick. There is no Input parameter on purpose: input reaches the simulation only as commands made in onInput, so a frame that runs zero or two ticks can neither lose nor repeat a press.
    virtual void onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) = 0;

    // Per rendered frame, after the ticks. alpha (0..1) is how far rendering is between the last two ticks.
    virtual void onUpdate(World& world, const Input& input, float frameSeconds, float alpha) = 0;

    // The camera the engine draws from. The engine sets its aspect ratio to match each frame.
    virtual Camera& getCamera() = 0;

    // True for a hidden, captured mouse that drives the camera; false for a visible cursor kept in the window.
    virtual bool wantsMouseLook() const = 0;
};

/*
* Run one simulation tick the way the engine does: remember transforms for interpolation, then let the game simulate.
* Shared by the engine loop and headless tests, so both run exactly the same steps.
*/
void runFixedUpdate(Game& game, World& world, float tickSeconds, double simulationSeconds);
