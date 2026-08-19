// Cinematics/TrackRegistry.h - the extensible track-kind registry.
//
// A Track's behaviour is not hard-coded in the evaluator; the Track carries a
// `kind` string that selects a registered TrackKind here. Built-in kinds (camera,
// transform, animation, event, audio, ...) are registered by
// RegisterBuiltinTrackKinds(); engine systems (or plugins) can register their own
// cinematic track types the same way, WITHOUT modifying the core evaluator or the
// sequence data model (spec §2). An unknown kind is skipped with a one-time warning
// so a sequence authored against a plugin track still loads.
#pragma once

#include "Core/Types.h"

#include <entt/entt.hpp>

#include <string>
#include <vector>

namespace hbe::cine {

struct Track;
struct EvalContext;
struct SequenceInstance;

// Everything a track kind needs to evaluate itself for one track this frame.
struct TrackEvalArgs {
    const Track& track;
    entt::entity target;   // resolved binding entity (entt::null when none/global)
    EvalContext& ctx;
    SequenceInstance& inst;
};

using TrackEvalFn = void (*)(const TrackEvalArgs&);

// A registered track type. `evaluate` applies the pose at ctx.t (idempotent);
// `fireEvents` fires events crossed in [ctx.prevT, ctx.t) (non-idempotent). Either
// may be null (a pure-event track has no evaluate; a pure-pose track has no
// fireEvents).
struct TrackKind {
    std::string id;        // registry key ("camera","transform","animation",...)
    std::string display;   // human label for the editor Add-Track menu
    std::string category;  // grouping ("Camera","Animation","Audio","Event",...)
    bool needsBinding = false; // requires a resolved target entity to do anything
    bool drivesCamera = false; // may claim the render camera
    bool isEvent = false;      // primarily an event track (editor draws it as markers)
    TrackEvalFn evaluate = nullptr;
    TrackEvalFn fireEvents = nullptr;
};

// Register a kind (replaces any existing kind with the same id).
void RegisterTrackKind(const TrackKind& kind);
// Look up a kind by id, or nullptr.
const TrackKind* FindTrackKind(const std::string& id);
// All registered kinds, in registration order (for the editor's Add-Track menu).
const std::vector<TrackKind>& TrackKinds();
// Register every built-in kind. Idempotent - safe to call from engine init AND
// each headless self-test.
void RegisterBuiltinTrackKinds();

} // namespace hbe::cine
