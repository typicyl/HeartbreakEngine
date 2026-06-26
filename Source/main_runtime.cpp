// main_runtime.cpp - Heartbreak Engine *game runtime* entry point.
//
// This build links the engine runtime only: NO editor, NO Dear ImGui. It is the
// executable a shipped game would use. It renders the scene straight to the
// window.
//
// The view is driven ENTIRELY by the scene's game camera (the primary
// CameraComponent, or whatever a CameraZone switches to): the runtime has no
// free-fly camera - that is an editor-only tool. The camera sticks to its
// authored behaviour (Static / First Person / Third Person / Orbit / ...), and
// player-controlled entities move via the CharacterController system.
//
// Usage: HeartbreakRuntime [--d3d12 | --vulkan] [--width N] [--height N]
//                          [--fullscreen | --windowed] [--validation] [--model <path>]
#include "Engine/Engine.h"

int main(int argc, char** argv) {
    hbe::EngineConfig config = hbe::ParseCommandLine(argc, argv);
    config.title = L"Heartbreak Engine (Runtime)";
    hbe::Engine engine;
    return engine.Run(config);
}
