// main_editor.cpp - Heartbreak *Editor* entry point.
//
// This build links the engine runtime PLUS the editor (Dear ImGui + ImGuizmo,
// asset browser, gizmos). It is a tool, not shipped with the game. It wires the
// editor UI into the engine via the engine's per-frame hook.
//
// Usage: HeartbreakEditor [--d3d12 | --vulkan] [--width N] [--height N]
//                         [--validation] [--model <path>]
#include "Assets/AssetFormats.h" // --test-assetformats (the registry's own invariants)
#include "Assets/AssetRefs.h"    // --test-packclosure (the pack dependency closure)
#include "Assets/SeamWeld.h"
#include "Assets/SlotIds.h"      // --test-slotids / --migrate-slots (pack slot identity)
#include "Assets/MusicGraph.h"   // --test-musicvoice (music director lifecycle)
#include "Assets/UAP.h"          // uap::PackIndexOf (the migration plan prints pack numbers)
#include "Audio/AudioSystem.h"   // --test-musicvoice
#include "Collab/CollabSelfTest.h"  // --test-collab
#include "Collab/Journal.h"          // --test-journal
#include "Collab/Identity.h"         // --test-identity
#include "Collab/SecureChannel.h"    // --test-securechannel
#include "Collab/ProjectSync.h"      // --test-projectsync
#include "Collab/WebRtcTransport.h"  // --test-webrtc
#include "Editor/CollabSession.h"   // --test-collabsession
#include "Scene/SceneJournal.h"       // --test-p2p
#include "Hub/HubConfig.h"           // --hub-install
#include "Hub/HubSelfTest.h"          // --test-hub
#include "Hub/Updater.h"              // --hub-check (live manifest fetch)
#include "Collab/TcpTransport.h"     // --test-tcp
#include "Core/JobSystem.h"
#include "Core/Window.h"
#include "Dialogue/DialogueGraph.h" // --test-graphfanin (node-graph reconvergence)
#include "Editor/Editor.h"
#include "Editor/Importer.h"
#include "Editor/MovieRender.h"
#include "Engine/Engine.h"
#include "Interaction/Pick.h" // --test-uipick (the interaction-raycast gate)
#include "Physics/PhysicsWorld.h"
#include "Project/Project.h"
#include "Renderer/Renderer.h"
#include "Scene/CameraSystem.h" // --test-fpslook (first-person look contract)
#include "Scene/EntityGuid.h"
#include "Scene/ParticleGpuSim.h"
#include "Scene/ParticleSystem.h"
#include "Scene/Scene.h"
#include "Scene/Hierarchy.h" // --test-pasteorder (the sibling-order contract)
#include "Scene/SceneSerializer.h"
#include "Scene/StrokeZone.h" // --test-strokezones (3D paint strokes stream with their zone)
#include "Scene/TagShard.h"
#include "Scene/TagStreaming.h"
#include "Scene/TagTable.h"
#include "UI/UIDocument.h"
#include "UI/UIManager.h" // --test-uiscreens (panel lookup across the screen set)
#include "UI/UISystem.h"  // --test-uisolve (direct-manipulation math gate)
#include "Vfx/VfxStack.h"

#include <glm/gtc/packing.hpp> // unpackHalf2x16 (GPU record colour)

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _DEBUG
#  include <crtdbg.h> // route Debug asserts to stderr - see the top of main()
#endif
#include <filesystem>
#include <fstream> // --test-readback raw frame dump / --test-readback-compare
#include <string>
#include <unordered_map> // legacy-.hbsave duplicate-guid check in --test-uiflow
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <imgui_impl_win32.h>

// Forward-declared per ImGui's documented pattern (declared inside `#if 0`).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
// Short, path-safe backend tag for the --test-readback frame dumps. NOT
// rhi::ToString, which returns "Direct3D 12" - a space in a filename that a
// sibling flag has to reconstruct exactly is a trap.
const char* ReadbackTag(hbe::rhi::GraphicsAPI api) {
    switch (api) {
        case hbe::rhi::GraphicsAPI::D3D12: return "d3d12";
        case hbe::rhi::GraphicsAPI::Vulkan: return "vulkan";
        case hbe::rhi::GraphicsAPI::OpenGL: return "opengl";
    }
    return "unknown";
}
} // namespace

int main(int argc, char** argv) {
#ifdef _DEBUG
    // A DEBUG BUILD MUST NEVER STOP ON A MODAL DIALOG. The CRT's default is to
    // pop "Debug Assertion Failed!" and wait forever, which turns any assert
    // during the ~35 headless `--test-*` flags into a HUNG process that looks
    // exactly like a slow one - a whole self-test sweep can sit there for hours
    // reporting nothing. Route asserts (and abort) to stderr instead, so the run
    // fails loudly, immediately, with the file and line, and exits non-zero.
    for (int mode : {_CRT_ASSERT, _CRT_ERROR, _CRT_WARN}) {
        _CrtSetReportMode(mode, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(mode, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    // --test-seamweld: run the modular-rig seam-weld bit-identity proof (headless,
    // no GPU/window) and exit. Used by CI / the build discipline.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--test-seamweld") == 0) {
            const bool ok = hbe::weld::SelfTest();
            std::printf("seamweld %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-assetformats: the ASSET REGISTRY's own invariants. Assets/
        // AssetFormats.cpp is the single source of truth for what an extension
        // means, and it now carries a second shipping contract beside
        // `runtimeLoaded`: how the pack closure walks the format. A row that
        // leaves that Unspecified is a format whose references silently do not
        // ship, so it is a build failure here rather than a mystery in a build.
        // Headless, no GPU/window/project. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-assetformats") == 0) {
            const bool ok = hbe::assets::RegistrySelfTest();
            std::printf("assetformats %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-packclosure: THE GATE for "Pack only referenced assets". Builds
        // a synthetic project exercising every format in the reference matrix,
        // each referencing the next, and proves the closure reaches all of them
        // (including a texture reachable only through .hbmat <- .hbprefab <-
        // .hbscene), that a real cook's packs contain exactly that set, that an
        // orphan is excluded with the filter on and present with it off, and
        // that an unresolvable or ambiguous reference is reported rather than
        // packed around. Headless, no GPU/window/project - it creates and
        // deletes its own scratch project. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-packclosure") == 0) {
            const bool ok = hbe::assets::PackClosureSelfTest();
            std::printf("packclosure %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-slotids: THE GATE for "an asset owns its pack slot for life".
        // Proves against a real scratch project and a real cook that a created
        // asset takes the next id, that deleting one frees EXACTLY that number
        // for the next creation, that no surviving asset ever moves, that
        // slot/50 picks the pack at the 49/50/51 boundary, that the migration is
        // idempotent, and that two cooks in a row produce byte-identical packs.
        // Headless, no GPU/window/project. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-slotids") == 0) {
            const bool ok = hbe::slots::SlotIdSelfTest();
            std::printf("slotids %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-vfxstack: prove the VFX attribute model + module-stack core
        // (64-byte record layout, dead-stream elimination, stage-order validation,
        // determinism) headless, no GPU/window. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-vfxstack") == 0) {
            const bool ok = hbe::vfx::SelfTest();
            std::printf("vfxstack %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-vfxcompat: prove that moving ParticleEmitter onto the module stack
        // changed nothing. Diffs the live path against a frozen copy of the pre-stack
        // simulation loop, bit-for-bit, over every preset plus a parameter fuzz.
        // Headless, no GPU/window. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-vfxcompat") == 0) {
            const bool ok = hbe::particle::CompatSelfTest();
            std::printf("vfxcompat %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-entityguid: prove the stable per-entity identity contract -
        // uniqueness across a scene, stability across save/load/save, FRESH guids
        // on copy/paste + prefab instantiate, and deterministic (stable across
        // reloads) assignment for a pre-guid scene file. Headless, no GPU/window.
        // Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-entityguid") == 0) {
            const bool ok = hbe::guid::SelfTest();
            std::printf("entityguid %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-noleveltypes: prove a level is ONE scene file - no UI scene kind,
        // a "<base>.static.hbscene"-NAMED file loads as an ordinary standalone
        // scene (its sibling ".dynamic" is not composed in), the per-object
        // Static/Dynamic tag the navmesh reads round-trips, and SaveScene ->
        // Parse -> SaveScene is byte-identical. Headless, no GPU/window. Same
        // contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-noleveltypes") == 0) {
            const bool ok = hbe::scene::LevelTypesSelfTest();
            std::printf("noleveltypes %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-paintcanvas: prove the paint-canvas save path - every canvas gets
        // a `source` (BuildSceneJson silently skips one that has none, which loses
        // the painting), colliding names get distinct files, an already-assigned
        // source is never reassigned, and the result reloads. Headless, no GPU.
        // --test-sceneslice: prove PARTIAL instantiation is correct - the thing all
        // of tag streaming stands on. One parsed scene is loaded twice, once whole
        // and once as two disjoint slices, and the two worlds must be the SAME world
        // (same entities by guid, byte-identical component state, same hierarchy)
        // except that a parent link crossing the slice boundary becomes a root.
        // Also pins blocker B1 (no Parent is ever emplaced on a null handle) and
        // blocker B2 (BindWorld applies the environment exactly once; a slice never
        // does; a second bind does not stack a second world). Headless, no GPU.
        if (std::strcmp(argv[i], "--test-sceneslice") == 0) {
            const bool ok = hbe::scene::SceneSliceSelfTest();
            std::printf("sceneslice %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-lightingparity: prove THE EDITOR AND THE SHIPPED GAME LIGHT A SCENE
        // THE SAME WAY. The same file is loaded through both real paths - the
        // editor's scene::LoadScene (LoadMode::Replace, what Editor::LoadSceneInEditor
        // calls) and the runtime's scene::BindWorld + Additive shard slices (what
        // stream::Streamer::BindLevel calls) - and the resulting SceneEnvironment must
        // be identical: ambientIntensity, exposure, shadowDistance, post (compared
        // whole) and giSource/giStatus/giOrigin/giSpacing/giDims plus giSh/giDepth
        // handle validity. Covers a scene WITH a baked `.hbgi` and one without, a
        // MISSING and a CORRUPT `.hbgi` (reported, and never inherited from the
        // previously loaded scene), shard-order independence, additive loads applying
        // no environment, re-bind idempotence, and that day/night MODULATES the
        // authored ambient/exposure rather than replacing them. Headless, no GPU/
        // window; creates its own scratch project. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-lightingparity") == 0) {
            const bool ok = hbe::scene::LightingParitySelfTest();
            std::printf("lightingparity %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-pasteorder: prove the SIBLING-ORDER CONTRACT (Scene/Hierarchy.h) -
        // a copied/duplicated/prefab-instantiated subtree reproduces the source's
        // child order at EVERY depth, wide and deep; clones still mint fresh guids;
        // a full save/load keeps the order over two cycles; a `.hbscene` written
        // before the "order" field loads exactly as it used to; and an explicit
        // drag-reorder is authored data that survives both. Includes the case the
        // OLD pool-order walk gets wrong (a registry whose Parent pool has been
        // perturbed by an unparent/reparent, plus recycled handles), and asserts
        // that the old walk really does disagree there - so the test measures the
        // fix rather than the status quo. Headless, no GPU/window/project.
        if (std::strcmp(argv[i], "--test-pasteorder") == 0) {
            const bool ok = hbe::scene::PasteOrderSelfTest();
            std::printf("pasteorder %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-fpslook: prove the FIRST-PERSON CAMERA LOOK contract. A mouse
        // delta produces the expected yaw/pitch at a known sensitivity, pitch
        // clamps at lookPitchMin/Max while yaw stays free, invertLookY flips it,
        // the right stick drives it as a RATE (scales with dt) while the mouse
        // does not, the CHARACTER's yaw follows the camera while its pitch never
        // does, the eye rides that yaw undamped, faceMoveDir stands down for the
        // frame the camera owns the body (with a positive control proving the
        // latch is one-shot), the cursor-lock gate suppresses look while a menu
        // or dialogue choice has freed the cursor, first and third person
        // accumulate look BIT-IDENTICALLY (they share one accumulator), aim
        // modes stand down for first-person look and only there, playerLook off
        // is exactly the old behaviour, and a scene round-trips every look field
        // while never serializing accumulated look state. Headless, no
        // GPU/window/project. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-fpslook") == 0) {
            const bool ok = hbe::cam::FirstPersonLookSelfTest();
            std::printf("fpslook %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-pasteparent: prove the PASTE-PARENTING CONTRACT - the other half of
        // "the clone lands where a human expects". A clipboard fragment cannot carry
        // its ROOT's parent (EntityToJson only writes a `parent` inside the subtree,
        // and the root's is by definition outside it), so a copy of a child used to
        // paste at the scene root - "it becomes its own thing". The parent is
        // captured at COPY time instead, and this pins what each caller does with it:
        // Ctrl+V/Ctrl+D produce a SIBLING of the source, LAST in that group; a copied
        // root stays a root; a `.hbprefab` drop is a root; prefab Revert restores the
        // INSTANCE's own parent, guid and sibling order. Plus every way the captured
        // handle goes bad - deleted since, a REPLACED world (valid but aliasing a
        // different entity), a handle aliasing something the paste itself created,
        // and a parent in another `.hbui` - each falling back to a root rather than
        // emplacing a dangling or boundary-crossing Parent. Includes the case the OLD
        // behaviour fails, so the test measures the fix rather than the status quo.
        // Headless, no GPU/window/project.
        if (std::strcmp(argv[i], "--test-pasteparent") == 0) {
            const bool ok = hbe::Editor::PasteParentSelfTest();
            std::printf("pasteparent %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-strokezones: prove that a 3D paint stroke belongs to the STREAMING
        // ZONE of the surface it was painted on (Scene/StrokeZone.h). A stroke on
        // tagged geometry lands in that zone's group node, the GROUP ITSELF carries
        // the tag - the only shape tagshard::Bake accepts, and the one this design
        // exists for - and the whole thing bakes with zero errors; a stroke on
        // untagged geometry falls back to the plain "Paint Strokes" group and stays
        // permanently resident; attaching never moves a stroke on screen; a scene
        // authored before zones ADOPTS its existing node instead of forking a second
        // one; the grouping survives save/load as a fixed point; a tagged stroke
        // survives a shard despawn/RESPAWN byte-identically (the blocker-B3 shape);
        // strokes are excluded from the navmesh while an identical ordinary prop is
        // not; and Rehome is idempotent. Headless: no GPU, no window, no project.
        if (std::strcmp(argv[i], "--test-strokezones") == 0) {
            const bool ok = hbe::strokezone::SelfTest();
            std::printf("strokezones %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-worldlocal: prove the WORLD-vs-LOCAL transform contract. Nav
        // steering, AI look-at and spawner placement each computed a WORLD-space
        // answer and assigned it to `Transform`, which is PARENT-RELATIVE - correct
        // for a root entity (which is why it survived), silently wrong for anything
        // parented to a moving platform, a room root or a streamed shard root. Pins
        // Scene::SetWorldPosition / SetWorldRotation against a rotated and
        // NON-UNIFORMLY SCALED parent, a two-level chain, the root identity case and
        // a Transform-less entity, and asserts in each case that the old raw
        // assignment FAILS - so the fixture is provably adversarial rather than
        // trivially satisfiable. Headless: no GPU, no window, no project.
        if (std::strcmp(argv[i], "--test-worldlocal") == 0) {
            const bool ok = hbe::scene::WorldLocalSelfTest();
            std::printf("worldlocal %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-graphfanin: prove RECONVERGENCE works in both node graphs. A choice
        // fanning out to several branches which then REJOIN a shared tail is the most
        // common shape in a visual scripting language, and both graphs made it
        // unauthorable: Connect enforced "one wire per INPUT pin" (correct for DATA
        // pins, wrong for EXEC), so wiring the second branch SILENTLY DELETED the
        // first and at runtime that branch hit Follow()==0 and ended early. Pins the
        // corrected rule - exec: output exclusive, input fan-in; data: input exclusive,
        // output fan-out - plus removal of a fan-in node, and a save/load round trip.
        // Every case asserts the OLD behaviour fails it. Headless.
        // --test-timelinesnap: prove the FRAME GRID every editor timeline now snaps
        // to. Before it, all three timelines wrote the raw mouse position into a
        // float-seconds key, so two keys meant to line up landed on 1.3871429s and
        // 1.3866667s and the music editor drew a bar grid it did not obey. Asserts
        // idempotence across eight frame rates over 0-3600s (a key is re-snapped on
        // every drag frame, on inspector release and again on save, so a Snap that
        // moved an already-snapped value would drift it every gesture), the
        // round-half-away-from-zero rule, the off-grid-duration clamp trap, and that
        // a disabled or Ctrl-suspended grid is EXACTLY the identity. Headless.
        // --test-projectkeys: prove a `.hbproj` survives a round trip through a build
        // that does not understand all of its keys. Project::Save() rebuilds the whole
        // file and used to start from an empty json object, emitting only the keys this
        // build knows - so every other key was silently DELETED on the first save. That
        // made the format lossy across engine versions in one direction, permanently:
        // open a project written by a newer build, change one setting, and every option
        // that build predates is gone. `j["version"] = 1` was written but never read, so
        // nothing detected the mismatch either. Also asserts the retired legacy keys
        // stay dropped (that drop IS the migration) and that a NEW project does not
        // inherit a previous one's unknown keys. Headless; writes only to temp.
        // --test-collab: drive the SHIPPING collaboration server and client over the
        // in-process loopback transport - no sockets, no ports, no threads, so a
        // two-client lock RACE is deterministic instead of flaky. Asserts: a contested
        // lock resolves to exactly one owner (never both, never neither); a lease
        // expires when its owner stops heartbeating and does NOT expire while it does;
        // a non-owner's edit is refused and never reaches authoritative state; a
        // stale-revision edit is detected; paint ops commit to the history in server
        // order with attribution while PREVIEWS never enter it; a reconnecting user
        // keeps their identity and reclaims their locks; and the framing survives
        // arbitrary stream splits, unknown message kinds and a hostile 4 GiB length.
        // Headless: no GPU, no window, no project.
        // --test-tcp: the same collaboration session over REAL localhost TCP, which is
        // the only way to exercise what the in-process loopback structurally cannot -
        // a partial send, a 64 KiB frame split across several recv() calls, and 200
        // small frames coalesced into one read. Binds an EPHEMERAL port (0) so it
        // cannot fail on a machine where something already owns a fixed one.
        // --test-hub: the launcher's update path, everything provable without a
        // network. Version ORDERING (a string compare puts 1.0.10 below 1.0.9 and
        // silently stops offering updates at the tenth patch), the real published
        // manifest shape, the https-only URL policy, the zip-slip containment guard,
        // the installer's refusals, and SHA-256 against its published test vectors.
        // --hub-check: a LIVE update check against the configured manifest URL, through
        // the real WinHTTP/TLS path. Separate from --test-hub on purpose: --test-hub must
        // never depend on a network, or a dropped wifi teaches people to ignore a red
        // test. This one is a diagnostic you run when you want to know about the server.
        // --hub-install <dir>: run a REAL install into <dir> through the shipping
        // installer - fetch the manifest, download, verify the hash, extract, swap, and
        // stamp. Exists because everything up to the swap was only ever exercised
        // against synthetic data; this is the one path that has to work on a stranger's
        // machine, and it deserves to be runnable without clicking through a GUI.
        if (std::strcmp(argv[i], "--hub-install") == 0 && i + 1 < argc) {
            hbe::hub::UpdatePaths ip;
            ip.installRoot = argv[i + 1];
            hbe::hub::Updater up("https://hollowdreamstudios.com/enginemanifest.json", ip);
            up.SetInstalledVersion(hbe::hub::ReadInstalledVersion(ip.installRoot));
            up.Check();
            std::printf("check: %s | %s\n", hbe::hub::UpdateStateName(up.Progress().state),
                        up.Progress().message.c_str());
            if (up.Progress().state != hbe::hub::UpdateState::Available) return 1;
            up.Apply([](const hbe::hub::UpdateProgress&) { return true; });
            const hbe::hub::UpdateProgress& p2 = up.Progress();
            std::printf("apply: %s | %s\n", hbe::hub::UpdateStateName(p2.state),
                        p2.message.c_str());
            const auto stamp = hbe::hub::ReadInstalledVersion(ip.installRoot);
            std::printf("stamp: %s | looksInstalled=%d\n",
                        stamp ? stamp->ToString().c_str() : "(none)",
                        hbe::hub::LooksInstalled(ip.installRoot) ? 1 : 0);
            return p2.state == hbe::hub::UpdateState::Done ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--hub-check") == 0) {
            wchar_t exe[MAX_PATH] = {};
            ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
            hbe::hub::UpdatePaths paths;
            paths.installRoot = std::filesystem::path(exe).parent_path().parent_path();
            hbe::hub::Updater up("https://hollowdreamstudios.com/enginemanifest.json", paths);
            up.Check();
            const hbe::hub::UpdateProgress& pr = up.Progress();
            std::printf("hub-check: state=%s local=%s remote=%s\n  %s\n",
                        hbe::hub::UpdateStateName(pr.state),
                        pr.localVersion.ToString().c_str(),
                        pr.remoteVersion.ToString().c_str(), pr.message.c_str());
            if (!pr.releaseUrl.empty())
                std::printf("  release: %s\n", pr.releaseUrl.c_str());
            return pr.state == hbe::hub::UpdateState::Failed ? 1 : 0;
        }
        // --test-componentdelta: the seam collaborative scene editing needs. Saving is
        // monolithic (EntityToJson writes a whole entity; Instantiate applies one while
        // CREATING it), and neither shape lets a client say "component C of an entity
        // that already exists becomes this". Proves every registered key round-trips
        // onto a DIFFERENT entity byte-for-byte, that omitted fields MERGE rather than
        // reset (resetting teleports objects), that an empty payload removes, and that
        // an unsupported key is REFUSED rather than silently ignored - a silent no-op
        // would be a divergence with no symptom until someone saved. Headless.
        if (std::strcmp(argv[i], "--test-componentdelta") == 0) {
            const bool ok = hbe::scene::ComponentDeltaSelfTest();
            std::printf("componentdelta %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-hub") == 0) {
            const bool ok = hbe::hub::HubSelfTest();
            std::printf("hub %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-tcp") == 0) {
            const bool ok = hbe::collab::TcpTransportSelfTest();
            std::printf("tcp %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-journal: the OFFLINE half of collaboration. A save seals a commit
        // (before/after bytes per entity+component against a named parent); going
        // offline is a fork and reconnecting is a fast-forward, a clean merge, or a
        // question for a human. Asserts the two properties that matter: a crash costs
        // only the LAST commit and never surfaces a partial one, and a merge between
        // two INDEPENDENTLY MIGRATED copies is refused - scene::MigrateSceneGuids
        // derives guids as a pure function of path and row index, so divergent copies
        // assign the same guid to different objects and a silent merge would move the
        // wrong things. Headless.
        // --test-p2p: THE WHOLE COLLABORATION STACK, end to end, against real scenes.
        // An elected-host session over a real transport (lock enforced, a non-owner
        // refused, an owner edit reaching the OTHER peer scene), then a peer going
        // offline, sealing its work as a commit, and reconciling - disjoint work
        // merging and landing without reverting local edits, an overlap held for
        // review with NOTHING applied, and independently-migrated copies refused.
        if (std::strcmp(argv[i], "--test-p2p") == 0) {
            const bool ok = hbe::scene::P2PEndToEndSelfTest();
            std::printf("p2p %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-identity: WHO a peer is, once the session is reachable from the open
        // internet. A per-install ECDSA P-256 keypair persists and stays stable, the
        // challenge never repeats, a genuine signature verifies - and impersonation,
        // replay, a tampered signature and tampered data all FAIL. Also asserts the
        // allowlist is default-DENY: an empty one admits nobody. Headless.
        if (std::strcmp(argv[i], "--test-identity") == 0) {
            const bool ok = hbe::collab::IdentitySelfTest();
            std::printf("identity %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-securechannel: the CHANNEL, over an untrusted network. A full TLS 1.3
        // handshake with no sockets, then every way it must fail - an unlisted peer
        // whose crypto is perfectly valid, a single flipped ciphertext byte, and an
        // intercepted / reflected / replayed identity proof. Headless.
        if (std::strcmp(argv[i], "--test-securechannel") == 0) {
            const bool ok = hbe::collab::SecureChannelSelfTest();
            std::printf("securechannel %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-webrtc: the PEER-TO-PEER path, over real ICE and a real data channel.
        // Invitation and reply exchanged as text, a direct link, mutual proof of
        // identity, then a whole collaboration session through it including a 200 KiB
        // blob that must survive chunking. Also asserts an uninvited peer holding a
        // genuine invitation never becomes a session. Hermetic: no ICE servers, so it
        // needs no internet.
        // --net-check: the DIAGNOSTIC for "it won't connect". Talks to real STUN servers
        // and reports what this machine looks like from outside. Needs the internet,
        // which is exactly why it is not part of --test-webrtc.
        if (std::strcmp(argv[i], "--net-check") == 0) {
            const bool ok = hbe::collab::NetCheck();
            return ok ? 0 : 1;
        }
        // --test-collabsession: the editor's FRONT DOOR, headlessly. What a save records
        // (and what it must leave out), a conflict held until a person answers it, and
        // the whole invite flow over a real peer-to-peer link - uninvited guest refused
        // but shown to the host, admitted, then connected. Hermetic: no ICE servers.
        if (std::strcmp(argv[i], "--test-collabsession") == 0) {
            const bool ok = hbe::editor::CollabSessionSelfTest();
            std::printf("collabsession %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-projectsync: handing a WHOLE project to a peer that has nothing.
        // What stays out (build output, caches, the host's own access list), that the
        // manifest is deterministic and content-addressed, that a transfer lands through
        // staging, and that an escaping path, an unoffered file, an oversized file and
        // wrong contents are each refused. Headless.
        if (std::strcmp(argv[i], "--test-projectsync") == 0) {
            const bool ok = hbe::collab::ProjectSyncSelfTest();
            std::printf("projectsync %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-webrtc") == 0) {
            const bool ok = hbe::collab::WebRtcSelfTest();
            std::printf("webrtc %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-journal") == 0) {
            const bool ok = hbe::collab::JournalSelfTest();
            std::printf("journal %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-collab") == 0) {
            const bool ok = hbe::collab::CollabSelfTest();
            std::printf("collab %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-projectkeys") == 0) {
            const bool ok = hbe::Project::ProjectKeysSelfTest();
            std::printf("projectkeys %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-timelinesnap") == 0) {
            const bool ok = hbe::editor::TimelineSnapSelfTest();
            std::printf("timelinesnap %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-graphfanin") == 0) {
            const bool ok = hbe::dlg::GraphFanInSelfTest();
            std::printf("graphfanin %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-shardstate: prove SHARD PERSISTENCE + manual spawn/despawn. A shard
        // is despawned and respawned and must come back with its door/kill/pickup/
        // trigger/destructible/AI/encounter state restored BY STABLE GUID (including
        // two entities that share a name, which the previous Name key collapsed onto
        // one row); a spawner's PROGRESS survives while its spawned population does
        // not (a cleared camp stays cleared, survivors reset - what keeps the save
        // bounded); no surviving component is left holding a dangling entt::entity;
        // a non-resident shard is never diffed as destroyed; and two full cycles are
        // stable in both entity count and stored-state size. Headless, no GPU.
        if (std::strcmp(argv[i], "--test-shardstate") == 0) {
            const bool ok = hbe::stream::SelfTest();
            std::printf("shardstate %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-tagpolicy: the DISTANCE-STREAMING DECISION rules, in isolation from
        // anything that can spawn. Proves distance is measured to the shard's AABB and
        // not its centre (the elongated-shard bug the deleted cell streamer had), that
        // a focus oscillating on the load boundary spawns ONCE - and that a degenerate
        // hysteresis band really does thrash, so the band is what stopped it - that
        // several foci union for loading and intersect for unloading, that priority
        // beats distance while distance breaks priority ties, that the concurrency
        // throttle and the unload cap both report unfinished work, that a focus inside a
        // shard can never unload it, that an empty focus list changes nothing, and that
        // "streaming off" pins everything LOADED. Pure data: no registry, no GPU, no
        // filesystem. Same contract as --test-seamweld.
        // --test-assoctags: ASSOCIATED TAGS (StreamPolicy.h RULE 6), end to end
        // through a real Streamer and by the author's own names. Three SEPARATE
        // pieces of content - City, City_LowPoly and Hill, each with its own
        // objects, bounds and radii - with Hill associating City_LowPoly. Proves
        // that standing on the hill makes the low-poly city resident 1.7 km outside
        // its own load radius; that leaving the hill releases it UNLESS it is in
        // range on its own; that a shard resident for both reasons survives losing
        // one (exactly one despawn, not a drop and a respawn); that a mutual
        // association TERMINATES and unloads instead of pinning the world; that an
        // association naming a tag that does not exist warns and streams on; and
        // that the added evaluation cost stays inside the streaming budget. Also
        // the bake's association diagnostics and the `.hbproj` round trip.
        //
        // It deliberately asserts NOTHING about City and City_LowPoly excluding each
        // other: they are separate assets and may be co-resident. That is the
        // author's choice, not the engine's business. Headless: no GPU, no window.
        if (std::strcmp(argv[i], "--test-assoctags") == 0) {
            const bool ok = hbe::stream::AssocSelfTest();
            std::printf("assoctags %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-editorzones: LIVE EDITOR ZONES - the streamer spawning and despawning
        // against the world the EDITOR is authoring. Proves the bind is
        // non-destructive (the entity set, every guid in it and Scene::WorldToken are
        // unchanged, so scene::BindWorld's DestroyWorld never ran); that a sweep
        // touches only streamed content and writes NO world::/game:: player-progress
        // state; that a manual override forces residency in both directions - out of a
        // zone the focus is standing inside, and into one 5 km away - and composes
        // with associations through the same seed set; that A SAVE NEVER WRITES A
        // PARTIAL WORLD (refused with the file untouched bytes AND mtime while a zone
        // is genuinely missing, allowed once the save path has settled the world,
        // still refused for a RUNTIME bind); that the Play/Stop snapshot round-trips
        // with streamed content present; and that a stream event never enters the undo
        // stack while an edit taken mid-stream still captures the whole world.
        //
        // Builds its own scratch project and level under the temp directory. Headless:
        // no GPU, no window, no ImGui context. Same contract as --test-scenesave, and
        // it never touches the user's project.
        if (std::strcmp(argv[i], "--test-editorzones") == 0) {
            const bool ok = hbe::Editor::EditorZoneSelfTest();
            std::printf("editorzones %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-tagpolicy") == 0) {
            const bool ok = hbe::stream::PolicySelfTest();
            std::printf("tagpolicy %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-paintcanvas") == 0) {
            const bool ok = hbe::scene::PaintCanvasSelfTest();
            std::printf("paintcanvas %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uidoc [<file.hbscene> ...]: the P2 gate. Proves the extracted
        // per-component UI JSON writers are BYTE-IDENTICAL to a frozen copy of
        // the blocks that used to be inlined in SceneSerializer.cpp (the
        // --test-vfxcompat pattern), that the .hbui round-trip is lossless
        // including the mandatory post header, and that the scene->document
        // converter partitions, remaps parents and strips non-document keys
        // without ever touching the source scene. Any .hbscene paths given are
        // additionally re-saved and diffed end-to-end against the frozen
        // writers. Headless, no GPU/window. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-uidoc") == 0) {
            std::vector<std::filesystem::path> scenes;
            for (int k = i + 1; k < argc; ++k) {
                if (argv[k][0] == '-') break; // next flag
                scenes.emplace_back(argv[k]);
            }
            const bool ok = hbe::ui::DocumentSelfTest(scenes);
            std::printf("uidoc %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uiseparation: prove the SEPARATION GUARANTEE's save-time half -
        // SaveSceneToDisk REFUSES (with a message, and without touching the file
        // on disk) when a non-document entity carries one of the six document
        // components, rather than silently dropping it. Also pins the deliberate
        // exemptions, including WorldText (level signage, not screen UI).
        // Headless, no GPU/window. Same contract as --test-seamweld.
        // --test-tagtable: the P4 gate for streaming tags. Proves interning
        // round-trips, that "Untagged" is index 0 and undeletable, that the
        // load/unload hysteresis band is enforced at parse (salvaged from the
        // deleted `.hbworld` manifest parser), that a project tag list round-trips
        // through the `.hbproj` (including present-but-empty and a repeated parse
        // into the same reused settings), that a per-entity tag survives
        // save/parse/save BYTE-IDENTICALLY, that assignment propagates over the
        // whole subtree, that deleting a tag remaps live entities, and that a
        // `.hbui` document entity CANNOT be tagged. Headless, no GPU/window. Same
        // contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-tagtable") == 0) {
            const bool ok = hbe::tags::SelfTest();
            std::printf("tagtable %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-shardbake: the P5 shard-bake gate. Proves the save-time SPATIAL
        // sharder is deterministic (shard indices are geometric, not row-order
        // derived), that every tagged entity lands in exactly one shard, that the
        // degenerate SCATTERED tag splits into many shards while an unsharded one is
        // reported as effectively always-loaded, that whole subtrees ride their root
        // and a cross-shard parent is REPORTED at bake time instead of being
        // discovered as a de-parented child at runtime, that shard AABBs contain
        // their members (brute-force cross-check, meshless volume entities included),
        // that the per-tag cap merges without dropping anything, and that the
        // "tagShards" file header round-trips and cross-checks against the file's own
        // entities. Headless, no GPU/window. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-shardbake") == 0) {
            const bool ok = hbe::tagshard::SelfTest();
            std::printf("shardbake %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-morphcache: the blocker-B3 gate. Proves blendshapes survive the
        // SECOND spawn of a mesh (they used to be lost for the rest of the session
        // because StageAssets skips a cache-resident model and the atlas was derived
        // only from freshly staged data), that no atlas is ever built or uploaded
        // twice - including on the mesh-collider path, which used to mint a fresh one
        // per respawn with no release - and that `.uaf` v8 persists morph targets at
        // all. Headless, no GPU/window.
        if (std::strcmp(argv[i], "--test-morphcache") == 0) {
            const bool ok = hbe::scene::MorphCacheSelfTest();
            std::printf("morphcache %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-uiseparation") == 0) {
            const bool ok = hbe::Editor::SeparationSelfTest();
            std::printf("uiseparation %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uisolve: the DIRECT-MANIPULATION math gate for the dedicated
        // `.hbui` editor's canvas (Source/Editor/UIEditor.cpp). Proves the
        // RectTransform solve is the exact inverse of the layout, that a solve
        // never touches anchors/pivot, that the "re-anchor by re-solving the same
        // rect" trick the anchor widget is built on is bit-stable, that snapping is
        // idempotent, and that ui::LayoutGroupOwnership agrees with what LayoutUI
        // actually did (so the editor never writes a rect the layout discards).
        // Headless, no GPU/window. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-uisolve") == 0) {
            const bool ok = hbe::ui::ManipulationSelfTest();
            std::printf("uisolve %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uieditor: the AUTHORING contract of that same panel's palette,
        // hierarchy and tools (phase I3). Palette creation joins the document
        // through DocumentSet::Track (tag AND order list), the no-document guard
        // creates nothing, a Z-ORDER reorder survives capture -> save -> reload
        // (the only honest test of it: draw order is entt pool order, which no file
        // records directly), reparent refuses across documents while allowing an
        // unparent, and a document-rooted subtree copy is a REAL fragment - the
        // regression that once let Cut delete content it had not copied.
        // Headless, no GPU/window/project. Same contract as --test-uiseparation.
        // --test-uipick: the INTERACTION-RAYCAST gate. Proves the unified pick pass
        // (Source/Interaction/Pick.cpp) is actually correct rather than merely
        // present: a page behind a wall is NOT clickable, exactly the nearer of two
        // overlapping pages receives the pointer, a rotated + non-uniformly scaled
        // (sheared) page maps to the canvas pixel an independent corner-based solve
        // says it should, a back-facing page stays inert, a mirrored page's
        // GEOMETRIC front face is the live one, an off-screen pointer picks nothing,
        // a page moving under a parent keeps picking correctly (surfaceInv
        // invalidation), and an Interactable and a page under the same reticle
        // produce exactly ONE winner - the nearer, occlusion-filtered, with the
        // proximity fallback preserved for "walk up and press E".
        // Headless: real Jolt, no GPU/window/project. Same contract as --test-uisolve.
        if (std::strcmp(argv[i], "--test-uipick") == 0) {
            const bool ok = hbe::interact::SelfTest();
            std::printf("uipick %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-3dinteract: the END-TO-END gate for 3D interactables, one layer up
        // from --test-uipick. A 3D button is a UIElement::Type::Button on a
        // world-space UICanvas (no third concept), so this drives the whole chain
        // the player touches - pointer source -> Pick -> ui::UpdateInteraction ->
        // hovered/held/clicked -> UIElement::action, the string schematics route On
        // UI Clicked on - in all three input modes: free cursor + LMB, locked-cursor
        // RETICLE + the Interact action, and a GAMEPAD (which aims the reticle in
        // every cursor state, because focus navigation deliberately never lands on a
        // world page). Plus: a wall blocks it, the TERRAIN HEIGHTFIELD blocks it,
        // nearest wins for page/page, page/object and object/object, and STREAMED
        // SHARD content behaves - a streamed-in Interactable becomes reachable, a
        // streamed-in collider starts occluding, and both stop on despawn.
        // Headless: real Jolt, a real baked level through the real stream::Streamer,
        // a device-less Renderer. No GPU/window/project.
        if (std::strcmp(argv[i], "--test-3dinteract") == 0) {
            const bool ok = hbe::interact::Interact3DSelfTest();
            std::printf("3dinteract %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-uieditor") == 0) {
            const bool ok = hbe::Editor::UIEditorSelfTest();
            std::printf("uieditor %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-savedispatch: prove the Ctrl+S DISPATCH RULE - each editing surface
        // maps to its own save target, exactly one target per keypress (no surface
        // but the scene's own can ever produce a scene write, which is the
        // double-fire bug this replaced), a focused-but-empty surface writes NOTHING
        // rather than falling through to the level, a focused text field DEFERS the
        // chord instead of dropping it, and Play refuses the two surfaces it mutates.
        // The decision is a pure function of a focused-surface id, so this needs no
        // ImGui context: headless, no GPU/window. Same contract as --test-seamweld.
        // Also covers the EDIT chords (Ctrl+Z/Y/X/C/V/D), which were an ungated
        // global poll until they were routed through the same claim model. `&` not
        // `&&` so both halves always run and both print their summary.
        if (std::strcmp(argv[i], "--test-savedispatch") == 0) {
            const bool saveOk = hbe::editor::SaveDispatchSelfTest();
            const bool editOk = hbe::editor::EditDispatchSelfTest();
            const bool ok = saveOk && editOk;
            std::printf("savedispatch %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
    }

    hbe::EngineConfig config = hbe::ParseCommandLine(argc, argv);
    config.title = L"Heartbreak Editor";

    // Normally no project is opened here - the editor's Project Manager modal
    // handles creating/opening projects (and remembers recent ones). The
    // --project flag opens one directly (automation / file association).
    if (!config.projectPath.empty()) {
        hbe::Project::Active().Open(std::filesystem::path(config.projectPath));
    }

    // Placed AFTER the project open so `--project` has taken effect (the music
    // graph and its audio assets resolve against Assets/).
    {
        for (int i = 1; i < argc; ++i) {
        // --test-musicvoice: drive the music director's full lifecycle - install a
        // graph, start every state, crossfade between them, stop, repeat - and assert
        // the layer list drains to zero.
        //
        // Context: a music layer's `ma_sound` used to be initialised in a STACK LOCAL
        // and then push_back-ed as a MOVE. ma_sound is self-referential (the node
        // graph holds pointers into it), so the graph kept pointing at the dead stack
        // frame and the miniaudio device thread faulted inside
        // ma_node_input_bus_read_pcm_frames. The DETERMINISTIC guard against that is
        // now a compile error - Voice has its copy/move members deleted - so this
        // test's job is the part a type cannot express: that the reap path actually
        // uninits and erases every layer instead of leaking them into the node graph,
        // and that repeated state changes stay stable. A leak here means the old
        // crash's sibling (an ever-growing node list) is back.
        if (std::strcmp(argv[i], "--test-musicvoice") == 0) {
            if (!hbe::Project::HasActive()) {
                std::printf("--test-musicvoice requires --project\n");
                return 1;
            }
            hbe::AudioSystem audio;
            if (!audio.IsAvailable()) {
                // Honest SKIP, not a green PASS: with no playback device this proves
                // nothing, and a vacuous pass is worse than no test.
                std::printf("musicvoice SKIP (no audio playback device)\n");
                return 0;
            }
            const auto& ms = hbe::Project::Active().Settings();
            if (ms.musicGraph.empty()) {
                std::printf("musicvoice SKIP (project has no musicGraph)\n");
                return 0;
            }
            const auto assetsDir = hbe::Project::Active().AssetsDir();
            const auto graph = hbe::assets::LoadMusicGraph(assetsDir / ms.musicGraph);
            if (!graph) {
                std::printf("musicvoice FAILED: could not load '%s'\n", ms.musicGraph.c_str());
                return 1;
            }
            audio.SetMusicGraph(*graph, assetsDir);
            const std::vector<std::string> states = audio.MusicStateNames();
            if (states.empty()) {
                std::printf("musicvoice SKIP (graph declares no states)\n");
                return 0;
            }
            // Pump ~1.2s of simulated frames; long enough for a short crossfade to
            // finish and the reaper to run.
            const auto pump = [&audio](int frames) {
                for (int f = 0; f < frames; ++f) {
                    audio.UpdateMusic(1.0f / 60.0f);
                    audio.Update();
                }
            };
            bool ok = true;
            for (int cycle = 0; cycle < 3; ++cycle) {
                for (const std::string& st : states) {
                    // A short explicit fade: the project's own default is 5.3s, which
                    // would outlast the pump and make the drain assertion meaningless.
                    audio.PlayMusicState(st, 0.05f);
                    pump(30);
                    if (audio.MusicLayerCount() == 0) {
                        std::printf("musicvoice FAILED: state '%s' started no layers\n",
                                    st.c_str());
                        ok = false;
                    }
                }
                audio.StopMusic(0.05f);
                pump(90);
                if (audio.MusicLayerCount() != 0) {
                    std::printf("musicvoice FAILED: %zu layer(s) survived the stop on "
                                "cycle %d (leaked into the node graph)\n",
                                audio.MusicLayerCount(), cycle);
                    ok = false;
                }
            }
            if (!audio.IsAvailable()) {
                std::printf("musicvoice FAILED: the audio engine died during the run\n");
                ok = false;
            }
            std::printf("musicvoice %s (%zu state(s), 3 cycles)\n", ok ? "PASS" : "FAILED",
                        states.size());
            return ok ? 0 : 1;
        }
        }
    }

    // --test-scenesave <scene.hbscene> [--project <proj>]: the SCENE-SAVE
    // COMPLETENESS CONTRACT - a .hbscene contains every entity of the active world,
    // or the save does not happen. Fixed-point + census-against-the-file-on-disk on
    // a real level, plus the refusals (despawned shards, a world replaced behind the
    // editor's back, an empty world over a populated file, Play mode) each asserted
    // on the target file's BYTES. Works on a COPY; never writes to the file named.
    //
    // Placed AFTER the project open so `--project` has taken effect (assets stage and
    // the tag table is seeded); headless otherwise - no GPU, no window, no ImGui.
    // A MISSING PATH FAILS rather than falling through to the editor.
    {
        bool want = false;
        const char* scenePath = nullptr;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--test-scenesave") != 0) continue;
            want = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') scenePath = argv[i + 1];
        }
        if (want) {
            std::filesystem::path p = scenePath ? std::filesystem::path(scenePath)
                                                : std::filesystem::path();
            if (!p.empty() && p.is_relative() && !std::filesystem::exists(p) &&
                hbe::Project::HasActive())
                p = hbe::Project::Active().AssetsDir() / p;
            const bool ok = hbe::Editor::SceneSaveSelfTest(p);
            std::printf("scenesave %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
    }

    // --migrate-guids [--dry-run]: freeze every guid-less entity's DERIVED guid into
    // the project's .hbscene files, then exit. See scene::MigrateSceneGuids for why
    // this is time-sensitive and why it is a surgical JSON edit rather than a
    // load/save round-trip.
    {
        bool migrate = false, dryRun = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--migrate-guids") == 0) migrate = true;
            if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
        }
        if (migrate) {
            if (!hbe::Project::HasActive()) {
                std::printf("--migrate-guids requires --project <file.hbproj>\n");
                return 1;
            }
            const auto st = hbe::scene::MigrateSceneGuids(
                hbe::Project::Active().AssetsDir(), dryRun);
            std::printf("migrate-guids %s: %u file(s), %u entity guid(s) stamped, "
                        "%u already had one, %u failed.\n",
                        dryRun ? "DRY RUN (nothing written)" : "done",
                        st.files, st.stamped, st.already, st.failed);
            return st.failed == 0 ? 0 : 1;
        }
    }

    // --migrate-slots [--dry-run] [--apply]: give every asset in the project a
    // permanent PACK SLOT and write it into the asset file, then exit. The logic
    // lives in the library (slots::MigrateSlotIds); this only calls it and prints.
    //
    // DRY RUN IS THE DEFAULT and `--apply` is what writes, because this rewrites
    // the author's own asset files - so the safe mode has to be the one you get
    // by typing the flag wrong. A bare --migrate-slots reports the entire plan,
    // every file and the id it would take, and stops without touching a byte.
    // (`--dry-run` is still accepted, and now redundant.)
    //
    // The order is chosen to make the FIRST cook after the migration the smallest
    // possible patch: an id the project's `.ship.uapmanifest` already records for
    // a still-present path is kept exactly, so those packs do not move at all;
    // everything else takes the lowest free id in sorted-path order. Re-running is
    // a no-op - the ids are in the files by then, and this recomputes the same
    // answer from them.
    {
        bool migrate = false, apply = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--migrate-slots") == 0) migrate = true;
            if (std::strcmp(argv[i], "--apply") == 0) apply = true;
        }
        if (migrate) {
            if (!hbe::Project::HasActive()) {
                std::printf("--migrate-slots requires --project <file.hbproj>\n");
                return 1;
            }
            const bool dryRun = !apply;
            const hbe::Project& proj = hbe::Project::Active();
            std::error_code sec;
            // Seed and target are now the SAME file: Project::SlotManifestPath is
            // the project's one slot ledger, and it is already the record of the
            // shipped layout (that is why it is the one that was kept). Passing it
            // as the seed as well is what makes `st.seeded` report how much of the
            // shipped layout the migration preserves.
            const std::filesystem::path seed =
                std::filesystem::exists(proj.SlotManifestPath(), sec) ? proj.SlotManifestPath()
                                                                      : std::filesystem::path();
            const auto st = hbe::slots::MigrateSlotIds(proj.AssetsDir(), proj.SlotManifestPath(),
                                                       seed, dryRun);
            std::printf("migrate-slots %s: %u packable asset(s), %u already had an id, "
                        "%u to stamp (%u seeded from '%s'), %u recorded in the manifest "
                        "only, %u collision(s), %u failed.\n",
                        dryRun ? "DRY RUN (nothing written)" : "done", st.scanned, st.already,
                        st.stamped, st.seeded,
                        seed.empty() ? "(no ship manifest)" : seed.filename().string().c_str(),
                        st.cannotEmbed, st.collisions, st.failed);
            // The plan itself - this is the artifact that makes the change
            // reviewable BEFORE it is applied, so it is printed in full rather
            // than summarised. Slot order, because that is pack order.
            for (const auto& [key, slot, seeded] : st.plan) {
                std::printf("  slot %-6u pack %-3u  %s%s\n", slot,
                            hbe::uap::PackIndexOf(slot), key.c_str(),
                            seeded ? "   (kept from the shipped layout)" : "");
            }
            if (dryRun) {
                std::printf("Nothing was written. Re-run with --apply to stamp these ids "
                            "into the asset files (put the project under version control "
                            "first).\n");
            }
            return st.failed == 0 ? 0 : 1;
        }
    }

    // --migrate-ui [--dry-run] [--force]: write a `.hbui` DOCUMENT for every
    // `.hbscene` in the project that contains UI, then exit. Same shape as
    // --migrate-guids above: the logic lives in the library
    // (ui::MigrateSceneUI), this only calls it and prints.
    //
    // IT NEVER DELETES OR REWRITES A SOURCE `.hbscene`. A fully-lifted scene is
    // reported as a RETIREMENT CANDIDATE and left exactly where it is; retiring
    // it is the operator's decision, and only after .hbproj points at the
    // document (P3). Re-running is a safe no-op: an existing destination is
    // skipped unless --force.
    {
        bool migrate = false, dryRun = false, force = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--migrate-ui") == 0) migrate = true;
            if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
            if (std::strcmp(argv[i], "--force") == 0) force = true;
        }
        if (migrate) {
            if (!hbe::Project::HasActive()) {
                std::printf("--migrate-ui requires --project <file.hbproj>\n");
                return 1;
            }
            // A migrated document's canvas block seeds from the PROJECT's canvas
            // configuration, because the legacy UI scenes lay their canvas-less
            // roots out against exactly that (Engine's per-frame uiConfig). The
            // converter never synthesises a UICanvas entity - that would move
            // those entities from the legacy walk to the canvas walk and change
            // both draw order and world-space routing.
            const hbe::BuildSettings& build = hbe::Project::Active().Settings().build;
            hbe::ui::CanvasConfig canvas;
            canvas.mode = static_cast<hbe::ui::ScaleMode>(
                std::clamp(build.uiScaleMode, 0u, 2u));
            canvas.refWidth = static_cast<hbe::f32>(std::max(build.uiRefWidth, 64u));
            canvas.refHeight = static_cast<hbe::f32>(std::max(build.uiRefHeight, 64u));

            const auto st = hbe::ui::MigrateSceneUI(hbe::Project::Active().AssetsDir(),
                                                    canvas, dryRun, force);
            std::printf("migrate-ui %s: %u .hbscene scanned, %u convertible, %u mixed, "
                        "%u UI entities, %u written, %u skipped, %u non-document keys "
                        "dropped, %u severed parents, %u stem collisions, %u failed. No "
                        "source scene was modified or deleted.\n",
                        dryRun ? "DRY RUN (nothing written)" : "done", st.files,
                        st.convertible, st.mixed, st.uiEntities, st.written, st.skipped,
                        st.droppedKeys, st.severedParents, st.collisions, st.failed);
            // Both new counters are LOSSY/REFUSED cases, so they are called out
            // rather than buried in the line above: a severed parent is a
            // world-mounted page that stopped following its mount (each one is
            // named in the per-file log), and a collision is a document that was
            // NOT written because two scenes with the same stem map to one
            // `UI/<stem>.hbui`.
            if (st.severedParents > 0)
                std::printf("migrate-ui: WARNING - %u UI entit(ies) lost a WORLD parent "
                            "and became document roots (see the log for names).\n",
                            st.severedParents);
            if (st.collisions > 0)
                std::printf("migrate-ui: WARNING - %u file(s) REFUSED on a destination "
                            "stem collision; nothing was overwritten.\n",
                            st.collisions);

            // Repoint the project's two UI slots at the generated documents. This
            // is the step that makes the migration take effect: until .hbproj
            // names the .hbui, the runtime keeps booting the legacy scenes through
            // the compatibility branch. It is a SURGICAL edit of four keys - see
            // ui::RepointProjectDocuments for why it is not a Project::Save().
            const int repointed = hbe::ui::RepointProjectDocuments(
                hbe::Project::Active().ProjectFile(), hbe::Project::Active().AssetsDir(),
                dryRun);
            if (repointed < 0) {
                std::printf("migrate-ui: FAILED to repoint the .hbproj.\n");
                return 1;
            }
            std::printf("migrate-ui: %d project slot(s) repointed%s. The source "
                        ".hbscene files are LEFT ON DISK and are now unreferenced "
                        "(retirement candidates - retiring them is your call).\n",
                        repointed, dryRun ? " (dry run)" : "");
            return st.failed == 0 ? 0 : 1;
        }
    }

    // --migrate-screens [--dry-run] [--force]: split each all-in-one `.hbui` in
    // the project's UI-screen list into ONE DOCUMENT PER SCREEN, then repoint the
    // `.hbproj` at the new set and exit. Same shape as --migrate-guids and
    // --migrate-ui above: the logic lives in the library (ui::SplitDocumentByPanel
    // + ui::RepointProjectScreens), this only calls it and prints.
    //
    // IT NEVER DELETES OR REWRITES A SOURCE DOCUMENT. The combined `.hbui` stays
    // exactly where it is, still loadable, and is reported as a retirement
    // candidate; retiring it is the operator's decision.
    //
    // DRY RUN IS THE DEFAULT, and `--apply` is what writes. This tool creates new
    // `.hbui` files AND rewrites the project's own `.hbproj` (ui::RepointProjectScreens
    // truncates and re-emits it), so the safe mode has to be the one you get by
    // typing the flag wrong. A bare --migrate-screens reports the whole plan -
    // screens, entity counts, destinations, orphans, action collisions, and the
    // JSON it would write - and stops without touching a byte. (`--dry-run` is
    // still accepted, and now redundant.)
    {
        bool migrate = false, apply = false, force = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--migrate-screens") == 0) migrate = true;
            if (std::strcmp(argv[i], "--apply") == 0) apply = true;
            if (std::strcmp(argv[i], "--force") == 0) force = true;
        }
        const bool dryRun = !apply;
        if (migrate) {
            if (!hbe::Project::HasActive()) {
                std::printf("--migrate-screens requires --project <file.hbproj>\n");
                return 1;
            }
            const hbe::Project& proj = hbe::Project::Active();
            const std::vector<std::string> sources = proj.Settings().uiDocuments;
            if (sources.empty()) {
                std::printf("migrate-screens: the project has no UI screen document "
                            "configured; nothing to split.\n");
                return 1;
            }
            const std::filesystem::path outDir = proj.AssetsDir() / "UI";
            std::vector<std::string> finalRels;
            bool anySplit = false, failed = false;
            for (const std::string& rel : sources) {
                const std::filesystem::path src = proj.AssetsDir() / rel;
                if (std::filesystem::path(rel).extension() != ".hbui") {
                    std::printf("migrate-screens: '%s' is not a .hbui (run "
                                "--migrate-ui first); left as-is.\n",
                                rel.c_str());
                    finalRels.push_back(rel);
                    continue;
                }
                hbe::ui::ScreenSplitReport rep;
                const bool ok = hbe::ui::SplitDocumentByPanel(src, outDir, "UI/", rep,
                                                              dryRun, force);
                if (ok && rep.screens.size() <= 1) {
                    // Already one screen per document - a clean no-op. Do not print
                    // the plan table: its lone row's "destination exists" is the
                    // file ITSELF and reads like a problem.
                    const std::string panel =
                        rep.screens.empty() ? std::string("no panel!") : rep.screens[0].panel;
                    std::printf("migrate-screens: '%s' already holds exactly one screen "
                                "(%s); nothing to split.\n",
                                rel.c_str(), panel.c_str());
                    finalRels.push_back(rel);
                    continue;
                }
                std::printf("migrate-screens %s: '%s' - %u source entities, %zu screen(s).\n",
                            dryRun ? "DRY RUN (nothing written)" : "done", rel.c_str(),
                            rep.sourceEntities, rep.screens.size());
                for (const auto& s : rep.screens)
                    std::printf("    %-14s -> %-28s %3u entities%s%s%s\n",
                                s.panel.c_str(), s.rel.c_str(), s.entities,
                                s.startVisible ? "  [startVisible]" : "",
                                s.existed ? "  [DESTINATION EXISTS]" : "",
                                s.wrote ? "  [written]" : "");
                if (rep.orphans > 0) {
                    std::printf("migrate-screens: WARNING - %u entit(ies) are under NO "
                                "root UIPanel and belong to no screen:\n",
                                rep.orphans);
                    for (const std::string& nm : rep.orphanNames)
                        std::printf("      %s\n", nm.c_str());
                }
                for (const std::string& d : rep.duplicatePanels)
                    std::printf("migrate-screens: ERROR - duplicate root panel name "
                                "'%s'.\n",
                                d.c_str());
                if (!rep.actionCollisions.empty()) {
                    std::printf("migrate-screens: WARNING - %zu UIElement::action value(s) "
                                "the engine resolves GLOBALLY appear in more than one "
                                "screen; all copies will fire/seed/write:\n",
                                rep.actionCollisions.size());
                    for (const std::string& a : rep.actionCollisions)
                        std::printf("      %s\n", a.c_str());
                }
                if (!ok) {
                    failed = true;
                    finalRels.push_back(rel);
                    continue;
                }
                if (rep.screens.size() <= 1) {
                    // Already one screen per document - leave the entry alone so
                    // re-running is a no-op rather than a churn.
                    finalRels.push_back(rel);
                    continue;
                }
                anySplit = true;
                for (const auto& s : rep.screens) finalRels.push_back(s.rel);
                std::printf("migrate-screens: '%s' is LEFT ON DISK, unmodified - it is "
                            "now a retirement candidate (retiring it is your call).\n",
                            rel.c_str());
            }
            if (failed) {
                std::printf("migrate-screens: FAILED; nothing was repointed.\n");
                return 1;
            }
            if (!anySplit) {
                std::printf("migrate-screens: nothing to do - every configured screen "
                            "document already holds exactly one screen.\n");
                return 0;
            }
            std::printf("migrate-screens: .hbproj uiDocuments would become:\n");
            for (std::size_t i = 0; i < finalRels.size(); ++i)
                std::printf("      [%zu] %s%s\n", i, finalRels[i].c_str(),
                            i == 0 ? "   (menu document - supplies `post`)" : "");
            if (!hbe::ui::RepointProjectScreens(proj.ProjectFile(), finalRels, dryRun)) {
                std::printf("migrate-screens: FAILED to repoint the .hbproj.\n");
                return 1;
            }
            std::printf("migrate-screens: %zu screen(s) referenced by the project%s.\n",
                        finalRels.size(), dryRun ? " (dry run - not written)" : "");
            if (dryRun)
                std::printf("migrate-screens: NOTHING WAS WRITTEN. Re-run with --apply "
                            "to create the screen documents and repoint the .hbproj.\n");
            return 0;
        }
    }

    // --pack / --ship: cook packs or the full shipping folder, then exit
    // (CI / scripted builds).
    if (config.packOnly || config.shipOnly) {
        hbe::jobs::Initialize(); // packing compresses assets across worker threads
        std::string msg;
        const bool ok = config.shipOnly ? hbe::Editor::BuildShipping(msg)
                                        : hbe::Editor::BuildAssetPack(msg);
        hbe::jobs::Shutdown();
        std::printf("%s\n", msg.c_str());
        return ok ? 0 : 1;
    }

    // --import <file>: import an asset into the project's Assets/ root, then
    // exit (automation / scripted content pipelines).
    if (!config.importPath.empty()) {
        if (!hbe::Project::HasActive()) {
            std::printf("--import requires --project\n");
            return 1;
        }
        const auto created = hbe::importer::Import(
            std::filesystem::path(config.importPath),
            hbe::Project::Active().AssetsDir());
        std::printf("import %s\n", created ? created->string().c_str() : "FAILED");
        return created ? 0 : 1;
    }

    // --test-readback: render a few editor frames offscreen at an ODD resolution,
    // read the frame back to CPU, write it to a PNG, and exit. Proves the GPU
    // readback path (row-pitch de-pad + canonical RGBA channel order) on the active
    // backend without needing to eyeball a live window.
    bool testReadback = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-readback") == 0) testReadback = true;
    if (testReadback) {
        if (!hbe::Project::HasActive()) {
            std::printf("--test-readback requires --project\n");
            return 1;
        }
        static hbe::Editor rbEditor;
        static int rbFrame = 0;
        // The verdict has to escape the frame lambda. This test used to `return
        // rbEngine.Run(config)` and only PRINT its own result, so it exited 0 no
        // matter what - a permanently green gate, and the only automated check
        // standing under --render-movie.
        static bool rbOk = false;
        hbe::Engine rbEngine;
        rbEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
            e.GetScene().SetEditorView(true);
            if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd))
                hbe::Editor::ApplyTheme();
        });
        rbEngine.SetOnFrame([](hbe::Engine& e) {
            rbEditor.BuildUI(e);
            constexpr hbe::u32 kW = 641, kH = 361; // odd -> exercises 256B row pitch
            e.GetRenderer().SetViewportSize(kW, kH);
            if (++rbFrame >= 30) {
                std::vector<hbe::u8> px;
                hbe::u32 w = 0, h = 0;
                const bool got = e.GetRenderer().ReadbackViewportColor(px, w, h);

                // ASSERT THE PIXELS, not just the bool. The three failures this test
                // exists to catch - a black frame, a channel swap, a row-pitch offset
                // - all return `true` from ReadbackViewportColor, so checking only
                // the bool could never have caught any of them.
                bool ok = got;
                const auto fail = [&ok](const char* why) {
                    std::printf("readback FAIL: %s\n", why);
                    ok = false;
                };
                if (!got) std::printf("readback FAIL: ReadbackViewportColor returned false\n");
                if (ok && (w != kW || h != kH)) fail("dimensions differ from the requested size");
                if (ok && px.size() != static_cast<hbe::usize>(w) * h * 4)
                    fail("buffer is not w*h*4 (row pitch not de-padded?)");
                if (ok) {
                    // Non-degenerate: a de-padded frame of a rendered scene is neither
                    // all zero nor one flat colour. A row-pitch bug that shifts rows
                    // still varies, so this is the weakest of the three checks - the
                    // strong one is the cross-backend compare below.
                    bool allZero = true, uniform = true;
                    for (hbe::usize i = 0; i < px.size(); ++i) {
                        if (px[i] != 0) allZero = false;
                        if (px[i] != px[i % 4]) uniform = false;
                        if (!allZero && !uniform) break;
                    }
                    if (allZero) fail("every byte is zero (black frame)");
                    else if (uniform) fail("every pixel is identical (nothing rendered?)");
                }

                // Per-backend filenames. Both backends used to write the SAME
                // hbe_readback.png, so running one after the other silently discarded
                // the first and no comparison was possible. A short tag, not
                // rhi::ToString - that yields "Direct3D 12", and a space in a path a
                // sibling tool has to reconstruct is a trap.
                const std::string api = ReadbackTag(e.GetRenderer().API());
                const auto dir = std::filesystem::temp_directory_path();
                const auto png = dir / ("hbe_readback_" + api + ".png");
                const auto raw = dir / ("hbe_readback_" + api + ".raw");
                if (got) {
                    hbe::movie::WritePng(png, w, h, px);
                    // Raw RGBA for --test-readback-compare (the D3D12<->Vulkan parity
                    // gate): PNG round-trips through an encoder, raw bytes do not.
                    std::ofstream rf(raw, std::ios::binary);
                    const hbe::u32 hdr[2] = {w, h};
                    rf.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
                    rf.write(reinterpret_cast<const char*>(px.data()),
                             static_cast<std::streamsize>(px.size()));
                }
                rbOk = ok;
                std::printf("readback %s %ux%u (%s) -> %s\n", ok ? "PASS" : "FAILED", w, h,
                            api.c_str(), png.string().c_str());
                e.Quit();
            }
        });
        const int runRc = rbEngine.Run(config);
        return (runRc == 0 && rbOk) ? 0 : 1;
    }

    // --test-readback-compare: the D3D12 <-> Vulkan parity gate. Reads the raw
    // frames --test-readback left in temp for each backend and compares them.
    //
    // "One-backend-only is a bug" is the stated rule of this engine's RHI seam, and
    // nothing mechanically enforced it - the two backends were only ever compared by
    // a human looking at two screenshots. Usage:
    //   HeartbreakEditor --project P --d3d12  --test-readback
    //   HeartbreakEditor --project P --vulkan --test-readback
    //   HeartbreakEditor --test-readback-compare
    bool testRbCompare = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-readback-compare") == 0) testRbCompare = true;
    if (testRbCompare) {
        const auto load = [](const char* api, hbe::u32& w, hbe::u32& h,
                             std::vector<hbe::u8>& px) -> bool {
            const auto p =
                std::filesystem::temp_directory_path() / ("hbe_readback_" + std::string(api) +
                                                          ".raw");
            std::ifstream f(p, std::ios::binary);
            if (!f) {
                std::printf("readback-compare: missing %s (run --test-readback --%s first)\n",
                            p.string().c_str(), api);
                return false;
            }
            hbe::u32 hdr[2] = {0, 0};
            f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
            w = hdr[0];
            h = hdr[1];
            px.assign(static_cast<hbe::usize>(w) * h * 4, 0);
            f.read(reinterpret_cast<char*>(px.data()),
                   static_cast<std::streamsize>(px.size()));
            return static_cast<hbe::usize>(f.gcount()) == px.size();
        };
        hbe::u32 aw = 0, ah = 0, bw = 0, bh = 0;
        std::vector<hbe::u8> a, b;
        if (!load("d3d12", aw, ah, a) || !load("vulkan", bw, bh, b)) return 1;
        if (aw != bw || ah != bh) {
            std::printf("readback-compare FAILED: %ux%u vs %ux%u\n", aw, ah, bw, bh);
            return 1;
        }
        // Not memcmp: two correct backends differ by rasterisation and filtering
        // rounding. A CHANNEL SWAP or a row-pitch shift moves the mean by far more
        // than that, which is what this is sized to catch.
        hbe::u64 diffSum = 0;
        hbe::u32 maxDiff = 0, badPixels = 0;
        for (hbe::usize i = 0; i < a.size(); ++i) {
            const hbe::u32 d = static_cast<hbe::u32>(std::abs(int(a[i]) - int(b[i])));
            diffSum += d;
            maxDiff = d > maxDiff ? d : maxDiff;
            if (d > 24) ++badPixels;
        }
        const double mean = a.empty() ? 0.0 : double(diffSum) / double(a.size());
        const double badPct = a.empty() ? 0.0 : 100.0 * double(badPixels) / double(a.size());
        // A swapped R/B channel on a sky gradient moves the mean by tens of levels;
        // legitimate backend rounding sits well under 1.
        const bool ok = mean < 4.0 && badPct < 2.0;
        std::printf("readback-compare %s: mean=%.3f max=%u over-threshold=%.2f%% (%ux%u)\n",
                    ok ? "PASS" : "FAILED", mean, maxDiff, badPct, aw, ah);
        return ok ? 0 : 1;
    }

    // --test-gpucompute: prove the general GPU-compute + GPU-writable-structured-
    // buffer seam on the ACTIVE backend (--d3d12 / --vulkan). Creates a CpuWrite SRV
    // buffer and a device-local ShaderWrite UAV buffer, queues a dispatch of
    // GpuComputeTest.hlsl, reads the UAV back, and checks every element. Needs a real
    // device (unlike --test-vfxstack), so it runs inside a short engine session and
    // needs no project. This is what stops the RHI plumbing from being delivered blind.
    bool testGpuCompute = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-gpucompute") == 0) testGpuCompute = true;
    if (testGpuCompute) {
        static constexpr hbe::u32 kN = 1024;
        struct GpuComputeTest {
            hbe::rhi::GpuBufferHandle in, out;
            hbe::rhi::ComputePipelineHandle pipe;
            int frame = 0;
            bool queued = false;
            bool done = false;
            bool pass = false;
            const char* why = "no result";
        };
        static GpuComputeTest t;
        hbe::Engine gcEngine;
        gcEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
        });
        gcEngine.SetOnFrame([](hbe::Engine& e) {
            auto& r = e.GetRenderer();
            if (t.done) return;
            if (++t.frame == 2) {
                if (!r.SupportsGpuCompute()) { t.why = "backend has no compute"; t.done = true; e.Quit(); return; }
                hbe::rhi::GpuBufferDesc inDesc{};
                inDesc.elementCount = kN;
                inDesc.elementStride = sizeof(hbe::u32);
                inDesc.usage = hbe::rhi::GpuBufferUsage::ShaderRead |
                               hbe::rhi::GpuBufferUsage::CpuWrite;
                inDesc.debugName = "GpuComputeTestIn";
                t.in = r.CreateGpuBuffer(inDesc);
                hbe::rhi::GpuBufferDesc outDesc{};
                outDesc.elementCount = kN;
                outDesc.elementStride = sizeof(hbe::u32);
                // ShaderRead + VertexBuffer too: exercises the exact usage combo the
                // GPU particle path needs (compute writes it, the VS reads it).
                outDesc.usage = hbe::rhi::GpuBufferUsage::ShaderWrite |
                                hbe::rhi::GpuBufferUsage::ShaderRead |
                                hbe::rhi::GpuBufferUsage::VertexBuffer;
                outDesc.debugName = "GpuComputeTestOut";
                t.out = r.CreateGpuBuffer(outDesc);
                hbe::rhi::ComputePipelineDesc pd{};
                pd.shaderName = "GpuComputeTest";
                pd.constantBytes = 16;
                pd.uavCount = 1;
                pd.srvCount = 1;
                t.pipe = r.CreateComputePipeline(pd);
                if (!t.in.IsValid() || !t.out.IsValid() || !t.pipe.IsValid()) {
                    t.why = "resource/pipeline creation failed";
                    t.done = true;
                    e.Quit();
                    return;
                }
                if (auto* src = static_cast<hbe::u32*>(r.MapGpuBuffer(t.in))) {
                    for (hbe::u32 i = 0; i < kN; ++i) src[i] = i * 7u + 3u;
                } else {
                    t.why = "MapGpuBuffer returned null";
                    t.done = true;
                    e.Quit();
                    return;
                }
                struct TestCB { hbe::u32 count, p0, p1, p2; } cb{kN, 0, 0, 0};
                hbe::rhi::ComputeDispatch d{};
                d.pipeline = t.pipe;
                d.constants = &cb;
                d.constantBytes = sizeof(cb);
                d.uavs[0] = t.out;
                d.uavCount = 1;
                d.srvs[0] = t.in;
                d.srvCount = 1;
                d.groupsX = (kN + 63) / 64; // numthreads(64,1,1)
                r.QueueCompute(d);           // executes at the next BeginFrame
                // Also exercise the VS-visible structured-buffer bind (D3D12 root
                // param 6 / Vulkan set 2); it must not disturb the scene pass.
                r.SetVertexShaderBuffer(t.out, 0);
                t.queued = true;
            } else if (t.queued && t.frame >= 5) {
                std::vector<hbe::u32> got(kN, 0);
                t.pass = r.ReadGpuBuffer(t.out, got.data(),
                                         static_cast<hbe::u32>(got.size() * sizeof(hbe::u32)));
                if (!t.pass) {
                    t.why = "ReadGpuBuffer failed";
                } else {
                    for (hbe::u32 i = 0; i < kN; ++i) {
                        const hbe::u32 want = (i * 7u + 3u) * 2u + 1u + (kN << 16);
                        if (got[i] != want) {
                            t.pass = false;
                            t.why = "element mismatch";
                            std::printf("  first mismatch at %u: got %u want %u\n", i, got[i], want);
                            break;
                        }
                    }
                    if (t.pass) t.why = "ok";
                }
                r.DestroyGpuBuffer(t.in);
                r.DestroyGpuBuffer(t.out);
                t.done = true;
                e.Quit();
            } else if (t.frame > 120) {
                t.why = "timed out";
                t.done = true;
                e.Quit();
            }
        });
        gcEngine.Run(config);
        std::printf("gpucompute %s (%s)\n", t.pass ? "PASS" : "FAIL", t.why);
        return t.pass ? 0 : 1;
    }

    // --test-uidoc-invariants <file.hbui>: the P3 STRUCTURAL contract. Every
    // entity a document creates carries UIDocMember and NONE carries world
    // content; both scene writers emit zero of them; a Replace sweep spares the
    // whole set while destroying an ordinary entity; capture round-trips; Close
    // reaps everything.
    //
    // GPU SESSION, not headless: opening a document runs ui::PreloadUIAssets,
    // which calls SharedFont().Initialize(renderer) and uploads every UI texture.
    // Same shape as --test-readback / --test-gpucompute - a short engine session
    // whose OnFrame does the work and quits. `--project` is required because the
    // preload resolves paths against the project's Assets/.
    {
        const char* docPath = nullptr;
        bool docFlagSeen = false;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], "--test-uidoc-invariants") == 0) {
                docFlagSeen = true;
                if (i + 1 < argc) docPath = argv[i + 1];
            }
        // The flag WITHOUT its argument used to fall straight through into the
        // interactive editor - so a scripted test sweep opened a window and hung
        // instead of reporting a usage error. Fail loudly like every other flag.
        if (docFlagSeen && !docPath) {
            std::printf("--test-uidoc-invariants requires a .hbui path\n");
            return 1;
        }
        if (docPath) {
            if (!hbe::Project::HasActive()) {
                std::printf("--test-uidoc-invariants requires --project\n");
                return 1;
            }
            static std::filesystem::path invPath;
            invPath = docPath;
            // Resolve a bare relative path against Assets/ for convenience.
            if (invPath.is_relative() && !std::filesystem::exists(invPath))
                invPath = hbe::Project::Active().AssetsDir() / invPath;
            static bool invRan = false, invOk = false;
            hbe::Engine invEngine;
            invEngine.SetOnInit([](hbe::Engine& e) {
                // Editor-shaped session: onInit_ is set, so the engine does NOT
                // open the project's boot/UI documents. The test opens its own.
                e.GetPhysics().SetRunning(false);
                e.SetGameCameraEnabled(false);
            });
            invEngine.SetOnFrame([](hbe::Engine& e) {
                if (invRan) return;
                invRan = true;
                invOk = hbe::ui::DocumentInvariantsSelfTest(e.GetScene(), &e.GetRenderer(),
                                                            invPath, /*preload*/ true);
                e.Quit();
            });
            invEngine.Run(config);
            std::printf("uidoc-invariants %s\n", invOk ? "PASS" : "FAIL");
            return invOk ? 0 : 1;
        }
    }

    // --test-uicanvas <file.hbui>: the ANTI-DRIFT GATE for the dedicated `.hbui`
    // editor's authoring canvas. That canvas's entire justification is that it is
    // the SHIPPED UI pass rather than a second renderer, and this asserts it
    // mechanically: the document-scoped build and the runtime build emit
    // BYTE-IDENTICAL vertex streams over the same scene, the document filter is
    // exact and inert, and the authoring render target is real and presentable to
    // ImGui. See ui::DocumentCanvasSelfTest for what it deliberately cannot cover
    // (whether the picture LOOKS right - that is a visual check).
    //
    // GPU SESSION, same shape as --test-uidoc-invariants: emission bakes fonts and
    // uploads UI textures, so `--project` is required.
    {
        const char* canvasPath = nullptr;
        bool canvasFlagSeen = false;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], "--test-uicanvas") == 0) {
                canvasFlagSeen = true;
                if (i + 1 < argc) canvasPath = argv[i + 1];
            }
        // Same fall-through-into-the-GUI hazard as --test-uidoc-invariants above.
        if (canvasFlagSeen && !canvasPath) {
            std::printf("--test-uicanvas requires a .hbui path\n");
            return 1;
        }
        if (canvasPath) {
            if (!hbe::Project::HasActive()) {
                std::printf("--test-uicanvas requires --project\n");
                return 1;
            }
            static std::filesystem::path canPath;
            canPath = canvasPath;
            if (canPath.is_relative() && !std::filesystem::exists(canPath))
                canPath = hbe::Project::Active().AssetsDir() / canPath;
            static bool canRan = false, canOk = false;
            hbe::Engine canEngine;
            canEngine.SetOnInit([](hbe::Engine& e) {
                // Editor-shaped session (onInit_ set), so the engine does not open
                // the project's boot/UI documents and pollute the scene the parity
                // check compares over.
                e.GetPhysics().SetRunning(false);
                e.SetGameCameraEnabled(false);
                e.GetScene().SetEditorView(true); // as the editor runs (EditorUIShow)
                // The panel hands its render target to ImGui, so check 4 needs a
                // real ImGui session - the same thing --test-readback does.
                if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd))
                    hbe::Editor::ApplyTheme();
            });
            canEngine.SetOnFrame([](hbe::Engine& e) {
                if (canRan) return;
                canRan = true;
                canOk = hbe::ui::DocumentCanvasSelfTest(e.GetScene(), e.GetRenderer(), canPath);
                e.Quit();
            });
            canEngine.Run(config);
            std::printf("uicanvas %s\n", canOk ? "PASS" : "FAIL");
            return canOk ? 0 : 1;
        }
    }

    // --test-uiflow: the RUNTIME game-flow contract, end to end on a real device.
    //
    // Booting -> MainMenu -> Loading -> Playing -> MainMenu, asserting the four
    // things P3 can silently break:
    //   1. the BOOT DOCUMENT IS CLOSED once boot finishes. Documents are spared
    //      by the sweep that used to dispose the splash implicitly, so without
    //      the explicit Close in FlowAfterBoot the splash renders forever over
    //      the menu - and nothing else in the engine would notice.
    //   2. exactly ONE UIPanel is active at a time, and
    //   3. it belongs to the bound UI document (FindPanel is document-scoped now;
    //      an unscoped one hands back a duplicate after a .hbsave restore).
    //   4. the document's entity count is UNCHANGED across a LoadGameplayWorld
    //      Replace and across a SaveGame/LoadGame cycle - the Replace sweep spares
    //      it, and BuildSceneJson never wrote it into the save in the first place.
    //
    // Runs the engine in RUNTIME mode (no OnInit hook), which is what makes the
    // boot sequence execute at all.
    bool testUIFlow = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-uiflow") == 0) testUIFlow = true;
    if (testUIFlow) {
        if (!hbe::Project::HasActive()) {
            std::printf("--test-uiflow requires --project\n");
            return 1;
        }
        struct UIFlowTest {
            int step = 0;
            int frame = 0;
            int stepFrame = 0;
            hbe::usize docCount = 0;
            bool pass = true;
            bool done = false;
            std::string why = "ok";
        };
        static UIFlowTest f;
        const auto fail = [](const char* why) {
            f.pass = false;
            f.why = why;
        };
        // Membership is in THE SCREEN SET now, not in one document: a per-screen
        // split makes "the UI document" four documents, and an assertion that
        // still named only the first would fail on three of them. The invariant
        // gets STRONGER, not weaker - "exactly one active panel across every
        // resident screen" also catches a second screen shipping a stray
        // startVisible, which the one-document world could not even express.
        static const auto inScreens = [](hbe::Engine& e, entt::entity ent) {
            const auto& reg = e.GetScene().Registry();
            const hbe::UIDocMember* m = reg.try_get<hbe::UIDocMember>(ent);
            if (!m) return false;
            for (const hbe::ui::DocHandle h : e.UIDocuments())
                if (h == m->doc) return true;
            return false;
        };
        static const auto countDoc = [](hbe::Engine& e) {
            const auto& reg = e.GetScene().Registry();
            hbe::usize n = 0;
            for (const entt::entity ent : reg.view<const hbe::UIDocMember>())
                if (inScreens(e, ent)) ++n;
            return n;
        };
        // Active panels, and whether every one of them is in the screen set.
        static const auto activePanels = [](hbe::Engine& e, bool& allInDoc) {
            const auto& reg = e.GetScene().Registry();
            hbe::usize n = 0;
            allInDoc = true;
            for (const entt::entity ent : reg.view<const hbe::UIPanel>()) {
                if (!reg.get<const hbe::UIPanel>(ent).active) continue;
                ++n;
                if (!inScreens(e, ent)) allInDoc = false;
            }
            return n;
        };
        hbe::Engine flowEngine;
        flowEngine.SetOnFrame([fail](hbe::Engine& e) {
            if (f.done) return;
            ++f.frame;
            ++f.stepFrame;
            if (f.frame > 3000) { // ~50 s at 60 Hz; the flow has real fades in it
                fail("timed out");
                f.done = true;
                e.Quit();
                return;
            }
            const auto advance = [&](int next) {
                f.step = next;
                f.stepFrame = 0;
            };
            switch (f.step) {
            case 0: // wait out the boot dwell
                if (e.State() == hbe::Engine::GameState::Booting) return;
                if (e.UIDocument() == 0) { fail("no UI document opened at boot"); break; }
                // (1) THE splash bug.
                if (e.BootDocument() != 0) {
                    fail("boot document still open after FlowAfterBoot "
                         "(the splash would render over the menu)");
                    break;
                }
                if (e.State() != hbe::Engine::GameState::MainMenu) {
                    fail("boot did not land in MainMenu");
                    break;
                }
                f.docCount = countDoc(e);
                if (f.docCount == 0) { fail("UI document has no live entities"); break; }
                {
                    bool inDoc = false;
                    const hbe::usize n = activePanels(e, inDoc);
                    if (n != 1) { fail("MainMenu: not exactly one active UIPanel"); break; }
                    if (!inDoc) { fail("MainMenu: the active panel is not in the UI document"); break; }
                }
                advance(1);
                return;
            case 1: // menu -> play
                e.FlowPlay();
                advance(2);
                return;
            case 2: // wait for the world + HUD
                if (e.State() != hbe::Engine::GameState::Playing) return;
                if (countDoc(e) != f.docCount) {
                    fail("document entity count changed across LoadGameplayWorld "
                         "(the Replace sweep did not spare it, or it was duplicated)");
                    break;
                }
                {
                    bool inDoc = false;
                    const hbe::usize n = activePanels(e, inDoc);
                    if (n != 1) { fail("Playing: not exactly one active UIPanel"); break; }
                    if (!inDoc) { fail("Playing: the active panel is not in the UI document"); break; }
                }
                advance(3);
                return;
            case 3: // save + load: the .hbsave must contain zero UI
                if (!e.SaveGame("__uiflowtest")) { fail("SaveGame failed"); break; }
                if (!e.LoadGame("__uiflowtest")) { fail("LoadGame failed"); break; }
                advance(4);
                return;
            case 4: {
                if (f.stepFrame < 3) return; // let the restore settle
                if (countDoc(e) != f.docCount) {
                    fail("document entity count changed across SaveGame/LoadGame "
                         "(UI leaked into the .hbsave)");
                    break;
                }
                // LEGACY (v1) `.hbsave` COMPATIBILITY, on real data. A save written before
                // tag streaming existed is a complete whole-world snapshot with no
                // "shards" key; loading it must restore that world, adopt whatever shard
                // membership the level describes, and above all not DOUBLE-SPAWN - the
                // failure mode of restoring a snapshot and then also spawning the shards
                // it already contains. Only runs when the project actually has one, so
                // this is a no-op on a fresh project rather than a false failure.
                if (e.HasSave("checkpoint")) {
                    if (!e.LoadGame("checkpoint")) {
                        fail("loading the project's existing (legacy) checkpoint.hbsave failed");
                        break;
                    }
                    const entt::registry& reg = e.GetScene().Registry();
                    std::unordered_map<hbe::u64, hbe::u32> perGuid;
                    for (const entt::entity ent : reg.view<const hbe::Guid>())
                        ++perGuid[reg.get<const hbe::Guid>(ent).value];
                    hbe::u32 dupes = 0;
                    for (const auto& [g, n] : perGuid)
                        if (n > 1) ++dupes;
                    if (dupes != 0) {
                        fail("a legacy checkpoint.hbsave restored DUPLICATE entities (the "
                             "double-spawn P7 exists to prevent)");
                        break;
                    }
                    if (countDoc(e) != f.docCount) {
                        fail("document entity count changed across a legacy .hbsave load");
                        break;
                    }
                }
                advance(5);
                return;
            }
            case 5: // back to the menu
                e.FlowMainMenu();
                advance(6);
                return;
            case 6:
                if (f.stepFrame < 3) return;
                if (e.State() != hbe::Engine::GameState::MainMenu) {
                    fail("quit-to-menu did not land in MainMenu");
                    break;
                }
                if (countDoc(e) != f.docCount) {
                    fail("document entity count changed across FlowMainMenu's sweep");
                    break;
                }
                {
                    bool inDoc = false;
                    const hbe::usize n = activePanels(e, inDoc);
                    if (n != 1) { fail("back at MainMenu: not exactly one active UIPanel"); break; }
                    if (!inDoc) { fail("back at MainMenu: the active panel is not in the UI document"); break; }
                }
                break;
            default: break;
            }
            f.done = true;
            e.Quit();
        });
        flowEngine.Run(config);
        if (!f.done) {
            f.pass = false;
            f.why = "the flow never completed";
        }
        // A TEST MAY NOT LEAVE A FILE IN THE USER'S PROJECT. This one exercises the
        // real SaveGame/LoadGame pair, which write `<project>/Saves/*.hbsave` - and
        // the flag REQUIRES --project, so every compliant regression sweep was
        // permanently dropping `__uiflowtest.hbsave` into whatever game it was run
        // against. The save path is the thing under test, so it stays; the artefact
        // does not. (Best-effort: a failure to remove it must not fail the test.)
        if (hbe::Project::HasActive()) {
            std::error_code rmec;
            std::filesystem::remove(
                hbe::Project::Active().Root() / "Saves" / "__uiflowtest.hbsave", rmec);
        }
        std::printf("uiflow %s (%s)\n", f.pass ? "PASS" : "FAIL", f.why.c_str());
        return f.pass ? 0 : 1;
    }

    // --test-uiscreens: THE GATE for one-.hbui-per-screen (task I1).
    //
    // Two halves. PHASE A is fully headless - no window, no GPU, no engine - and
    // pins everything that is a property of the FILES:
    //   1. every configured screen document LOADS INDEPENDENTLY (each one is a
    //      complete, self-describing `.hbui`: kind, canvas config, post block),
    //   2. a split document ROUND-TRIPS byte-for-byte through the in-memory form
    //      (LoadDocumentFromString(SaveDocumentToString(d)) == d as text),
    //   3. panel names are UNIQUE across the set - a duplicate makes Show(name)
    //      ambiguous and UIManager::Init reject the second,
    //   4. the GLOBALLY-RESOLVED UIElement::action values (`setting:*` plus the
    //      flow verbs and "caption") are unique across the set. This is the ONE
    //      new invariant the split can violate: every consumer of `action`
    //      addresses it by string over the whole registry, so a `setting:volume`
    //      in two screens would be seeded twice and written twice.
    //   5. every screen document declares at least one root UIPanel - a screen
    //      file with no panel can never be shown by name.
    //
    // PHASE B runs the real runtime boot and pins the FLOW: all screens resident,
    // the manager reaches EACH ONE by name (Show/Push/Pop), exactly one panel is
    // ever active, the resident entity count never moves (residency, not
    // on-demand loading), the preload contract held (no unresolved texture on a
    // screen that has just been shown - the "no white quads" guarantee), and the
    // flow still lands MainMenu -> Playing/HUD -> MainMenu.
    bool testUIScreens = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-uiscreens") == 0) testUIScreens = true;
    if (testUIScreens) {
        if (!hbe::Project::HasActive()) {
            std::printf("--test-uiscreens requires --project\n");
            return 1;
        }
        bool passA = true;
        const auto failA = [&passA](const std::string& why) {
            passA = false;
            std::printf("uiscreens: FAIL - %s\n", why.c_str());
        };

        const hbe::Project& proj = hbe::Project::Active();
        const std::vector<std::string>& rels = proj.Settings().uiDocuments;
        if (rels.empty()) failA("the project configures no UI screen documents");

        std::unordered_map<std::string, std::string> panelOwner;  // panel -> screen rel
        std::unordered_map<std::string, std::string> actionOwner; // action -> screen rel
        // ONE definition (ui::IsGlobalAction) shared with the boot audit and the
        // migrator's collision report - a local copy could drift out of lockstep
        // and silently stop gating.
        const auto isGlobalAction = [](const std::string& a) {
            return hbe::ui::IsGlobalAction(a);
        };
        std::size_t totalPanels = 0;
        for (const std::string& rel : rels) {
            const std::filesystem::path p = proj.AssetsDir() / rel;
            if (std::filesystem::path(rel).extension() != ".hbui") {
                std::printf("uiscreens: '%s' is a LEGACY scene slot; skipping the "
                            "document checks for it.\n",
                            rel.c_str());
                continue;
            }
            // (1) loads independently
            hbe::ui::DocData d;
            if (!hbe::ui::LoadDocument(p, d)) {
                failA("screen '" + rel + "' failed to load on its own");
                continue;
            }
            if (d.entities.empty()) failA("screen '" + rel + "' has no entities");
            if (d.canvas.refWidth <= 0.0f || d.canvas.refHeight <= 0.0f)
                failA("screen '" + rel + "' has no usable canvas config - it is not "
                                         "self-describing, so its layout depends on "
                                         "whatever document loaded first");
            // (2) round-trips
            const std::string once = hbe::ui::SaveDocumentToString(d);
            hbe::ui::DocData back;
            if (!hbe::ui::LoadDocumentFromString(once, back)) {
                failA("screen '" + rel + "' does not re-parse from its own text");
            } else if (hbe::ui::SaveDocumentToString(back) != once) {
                failA("screen '" + rel + "' does not round-trip byte-for-byte");
            }
            // (3) + (5) panels
            std::size_t rootsHere = 0;
            for (const hbe::ui::DocEntity& e : d.entities) {
                if (!e.hasPanel) continue;
                if (e.parent < 0) ++rootsHere;
                ++totalPanels;
                const auto [it, fresh] = panelOwner.emplace(e.panel.name, rel);
                if (!fresh)
                    failA("panel name '" + e.panel.name + "' appears in BOTH '" +
                          it->second + "' and '" + rel +
                          "'; Show(name) would be ambiguous");
            }
            if (rootsHere == 0)
                failA("screen '" + rel +
                      "' declares no ROOT UIPanel, so it can never be shown by name");
            // (4) globally-resolved actions
            for (const hbe::ui::DocEntity& e : d.entities) {
                if (!e.hasElement) continue;
                const std::string& a = e.element.action;
                if (a.empty() || !isGlobalAction(a)) continue;
                const auto [it, fresh] = actionOwner.emplace(a, rel);
                if (!fresh && it->second != rel)
                    failA("action '" + a + "' is resolved GLOBALLY by the engine but "
                          "appears in BOTH '" + it->second + "' and '" + rel +
                          "'; both copies would fire");
            }
        }
        if (totalPanels == 0) failA("no UIPanel found in any configured screen");
        std::printf("uiscreens phase A: %zu screen document(s), %zu panel(s), %zu "
                    "globally-resolved action(s).\n",
                    rels.size(), totalPanels, actionOwner.size());
        if (!passA) {
            std::printf("uiscreens FAIL (phase A - the files)\n");
            return 1;
        }

        // ---- Phase B: the live flow over the resident screen set --------------
        struct ScreensTest {
            int step = 0;
            int frame = 0;
            int stepFrame = 0;
            hbe::usize residentEntities = 0;
            std::size_t screenIdx = 0;
            std::vector<std::string> panels; // every panel name, discovered at boot
            std::string restore;             // panel to put back when done cycling
            bool pass = true;
            bool done = false;
            std::string why = "ok";
        };
        static ScreensTest s;
        s = ScreensTest{};
        const auto fail = [](const char* why) {
            s.pass = false;
            s.why = why;
        };
        // Resident members of the SCREEN SET.
        static const auto residentCount = [](hbe::Engine& e) {
            const auto& reg = e.GetScene().Registry();
            hbe::usize n = 0;
            for (const entt::entity ent : reg.view<const hbe::UIDocMember>()) {
                const hbe::u32 doc = reg.get<const hbe::UIDocMember>(ent).doc;
                for (const hbe::ui::DocHandle h : e.UIDocuments())
                    if (h == doc) { ++n; break; }
            }
            return n;
        };
        static const auto activeCount = [](hbe::Engine& e) {
            const auto& reg = e.GetScene().Registry();
            hbe::usize n = 0;
            for (const entt::entity ent : reg.view<const hbe::UIPanel>())
                if (reg.get<const hbe::UIPanel>(ent).active) ++n;
            return n;
        };
        // THE PRELOAD GUARANTEE, as an assertion: after a screen is shown, every
        // element in it that names a texture must already have it resolved. An
        // unresolved reference is exactly the white quad / blank glyph a
        // load-on-demand design would flash.
        static const auto unresolvedIn = [](hbe::Engine& e, const std::string& panel) {
            auto& reg = e.GetScene().Registry();
            const entt::entity root = e.GetUIManager().PanelEntity(e.GetScene(), panel);
            if (root == entt::null) return hbe::usize(0);
            hbe::usize bad = 0;
            for (const entt::entity ent : reg.view<hbe::UIElement>()) {
                entt::entity cur = ent;
                bool under = false;
                for (int d = 0; cur != entt::null && d < 64; ++d) {
                    if (cur == root) { under = true; break; }
                    const hbe::Parent* p = reg.try_get<hbe::Parent>(cur);
                    cur = (p && reg.valid(p->entity)) ? p->entity : entt::null;
                }
                if (!under) continue;
                const hbe::UIElement& el = reg.get<hbe::UIElement>(ent);
                if (!el.texture.empty() && !el.textureResolved) ++bad;
            }
            return bad;
        };

        hbe::Engine screensEngine;
        screensEngine.SetOnFrame([fail, &rels](hbe::Engine& e) {
            if (s.done) return;
            ++s.frame;
            ++s.stepFrame;
            if (s.frame > 3000) {
                fail("timed out");
                s.done = true;
                e.Quit();
                return;
            }
            const auto advance = [&](int next) {
                s.step = next;
                s.stepFrame = 0;
            };
            switch (s.step) {
            case 0: // wait out the boot dwell
                if (e.State() == hbe::Engine::GameState::Booting) return;
                if (e.BootDocument() != 0) {
                    fail("boot document still open after FlowAfterBoot");
                    break;
                }
                // ALL screens resident, one open document each.
                if (e.UIDocuments().size() != rels.size()) {
                    fail("not every configured screen document is resident");
                    break;
                }
                if (e.State() != hbe::Engine::GameState::MainMenu) {
                    fail("boot did not land in MainMenu");
                    break;
                }
                if (activeCount(e) != 1) {
                    fail("MainMenu: not exactly one active UIPanel across the screen "
                         "set (a second screen shipped a stray startVisible?)");
                    break;
                }
                s.residentEntities = residentCount(e);
                if (s.residentEntities == 0) { fail("the screen set has no entities"); break; }
                // Collect every panel name the manager can reach.
                {
                    auto& reg = e.GetScene().Registry();
                    for (const entt::entity ent : reg.view<const hbe::UIPanel>()) {
                        const hbe::UIDocMember* m = reg.try_get<hbe::UIDocMember>(ent);
                        if (!m) continue;
                        bool mine = false;
                        for (const hbe::ui::DocHandle h : e.UIDocuments())
                            if (h == m->doc) { mine = true; break; }
                        if (!mine) continue;
                        const std::string& nm = reg.get<const hbe::UIPanel>(ent).name;
                        if (!nm.empty()) s.panels.push_back(nm);
                    }
                    std::sort(s.panels.begin(), s.panels.end());
                    s.restore = e.GetUIManager().Top();
                }
                if (s.panels.empty()) { fail("no reachable panels"); break; }
                // THE INITIAL SCREEN MUST EXIST AND BE REACHABLE. Only ONE of the
                // split documents carries `startVisible`, so losing that one file
                // leaves `initial_` empty, ShowInitial a silent no-op and the game
                // parked in MainMenu with a black screen and zero elements.
                // Nothing checked this before.
                {
                    const std::string init = e.GetUIManager().Initial();
                    if (init.empty()) {
                        fail("the UIManager has NO initial screen (nothing declares "
                             "startVisible, and the boot fallback did not fire)");
                        break;
                    }
                    if (std::find(s.panels.begin(), s.panels.end(), init) ==
                        s.panels.end()) {
                        fail(("the initial screen '" + init +
                              "' is not a reachable panel in the resident set")
                                 .c_str());
                        break;
                    }
                    if (e.GetUIManager().Top() != init) {
                        fail(("boot showed '" + e.GetUIManager().Top() +
                              "' but the initial screen is '" + init + "'")
                                 .c_str());
                        break;
                    }
                }
                advance(1);
                return;
            case 1: { // show EVERY screen in turn - each must be reachable by name
                if (s.screenIdx >= s.panels.size()) {
                    e.GetUIManager().Show(e.GetScene(),
                                          s.restore.empty() ? s.panels.front() : s.restore);
                    advance(2);
                    return;
                }
                const std::string& nm = s.panels[s.screenIdx];
                if (s.stepFrame == 1) {
                    if (!e.GetUIManager().Has(e.GetScene(), nm)) {
                        fail("a panel in the screen set is not reachable by name");
                        break;
                    }
                    e.GetUIManager().Show(e.GetScene(), nm);
                    return; // let the show land before asserting
                }
                if (activeCount(e) != 1) {
                    fail("showing a screen left more than one active UIPanel");
                    break;
                }
                if (e.GetUIManager().Top() != nm) {
                    fail("Show(name) did not make that screen the top of the stack");
                    break;
                }
                // RESIDENCY: showing a screen must not load or destroy anything.
                if (residentCount(e) != s.residentEntities) {
                    fail("the resident entity count moved when a screen was shown - "
                         "screens are supposed to be resident, not loaded on demand");
                    break;
                }
                // PRELOAD: nothing unstyled on the frame it appears.
                if (unresolvedIn(e, nm) != 0) {
                    fail("a shown screen has an UNRESOLVED texture reference (it would "
                         "flash a white quad) - the preload contract was not honoured");
                    break;
                }
                ++s.screenIdx;
                s.stepFrame = 0;
                return;
            }
            case 2: // and the real flow still works end to end
                e.FlowPlay();
                advance(3);
                return;
            case 3:
                if (e.State() != hbe::Engine::GameState::Playing) return;
                if (residentCount(e) != s.residentEntities) {
                    fail("the screen set's entity count changed across "
                         "LoadGameplayWorld (a Replace sweep did not spare it)");
                    break;
                }
                if (activeCount(e) != 1) { fail("Playing: not exactly one active UIPanel"); break; }
                if (e.GetUIManager().Top() != "HUD") {
                    fail("Playing did not put the HUD screen on top");
                    break;
                }
                advance(4);
                return;
            case 4: // settings must still PUSH over the HUD and POP back
                e.GetUIManager().Push(e.GetScene(), "Settings");
                advance(5);
                return;
            case 5:
                if (s.stepFrame < 2) return;
                if (e.GetUIManager().Top() != "Settings") {
                    fail("Push(\"Settings\") did not reach the Settings screen "
                         "(a cross-document panel lookup failed)");
                    break;
                }
                if (activeCount(e) != 1) { fail("Settings: not exactly one active UIPanel"); break; }
                e.GetUIManager().Pop(e.GetScene());
                advance(6);
                return;
            case 6:
                if (s.stepFrame < 2) return;
                if (e.GetUIManager().Top() != "HUD") { fail("Pop did not restore the HUD"); break; }
                e.FlowMainMenu();
                advance(7);
                return;
            case 7:
                if (s.stepFrame < 3) return;
                if (e.State() != hbe::Engine::GameState::MainMenu) {
                    fail("quit-to-menu did not land in MainMenu");
                    break;
                }
                if (residentCount(e) != s.residentEntities) {
                    fail("the screen set's entity count changed across FlowMainMenu's "
                         "sweep");
                    break;
                }
                if (activeCount(e) != 1) {
                    fail("back at MainMenu: not exactly one active UIPanel");
                    break;
                }
                break;
            default: break;
            }
            s.done = true;
            e.Quit();
        });
        screensEngine.Run(config);
        if (!s.done) {
            s.pass = false;
            s.why = "the flow never completed";
        }
        std::printf("uiscreens %s (%s)\n", s.pass ? "PASS" : "FAIL", s.why.c_str());
        return s.pass ? 0 : 1;
    }

    // --test-vfxsim: CPU/GPU PARITY for the module-stack interpreter.
    //
    // Shaders/VfxSim.hlsl is a transliteration of the K_* kernels in VfxStack.cpp -
    // same modules, same order, same RNG draw order - so the two paths must agree.
    // This drives ONE emitter on the real GPU path (the engine's own particle::Update
    // -> particle::GpuSim -> compute dispatch), drives a CPU reference through
    // vfx::RunFrame with the SAME compiled stack and the SAME per-frame ModuleParams
    // (taken from the live emitter, so the test cannot pass by testing its own copy of
    // the mapping), reads the simulation buffer back, and compares particle for
    // particle.
    //
    // NOT bit-exact, deliberately: sin/cos/exp are implementation-defined and glm::mix
    // is x*(1-a)+y*a where HLSL lerp is x+a*(y-x). The assertion is a relative
    // tolerance plus a MOVEMENT check - a comparison that passes because both sides
    // produced zeros would prove nothing.
    bool testVfxSim = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-vfxsim") == 0) testVfxSim = true;
    if (testVfxSim) {
        static constexpr hbe::f32 kDt = 1.0f / 60.0f;
        static constexpr hbe::u32 kBurst = 256;   // one shot: slot k == pool index k
        static constexpr hbe::u32 kSimFrames = 45;
        struct VfxSimTest {
            entt::entity entity = entt::null;
            hbe::vfx::CompiledStack refStack;
            hbe::vfx::ParticleSoA refPool;
            hbe::vfx::EmitterState refState;
            hbe::u32 refFrames = 0;
            bool armed = false;
            bool settled = false;
            int frame = 0;
            bool done = false;
            bool pass = false;
            const char* why = "no result";
            hbe::f32 worstRel = 0.0f;
            hbe::f32 travelled = 0.0f;
            hbe::u32 compared = 0;
        };
        static VfxSimTest v;
        hbe::Engine vsEngine;
        vsEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
            e.SetRenderFixedDt(kDt); // determinism: the CPU reference uses the same dt
        });
        vsEngine.SetOnFrame([](hbe::Engine& e) {
            if (v.done) return;
            auto& scene = e.GetScene();
            auto& reg = scene.Registry();
            auto& r = e.GetRenderer();
            ++v.frame;

            if (v.frame == 2) {
                if (!r.SupportsGpuCompute()) {
                    v.why = "backend has no compute";
                    v.done = true;
                    e.Quit();
                    return;
                }
                v.entity = scene.CreateEntity("VfxSimTest");
                hbe::Transform t;
                t.position = {3.5f, 1.25f, -2.0f}; // non-trivial world matrix
                t.rotation = glm::quat(glm::vec3(0.35f, 0.8f, -0.2f)); // yaw/pitch/roll
                reg.emplace<hbe::Transform>(v.entity, t);

                hbe::ParticleEmitter em;
                em.gpuSim = true;
                em.gpuSeed = 0x1234ABCDu; // fixed so the reference can use it too
                // One shot, nothing retires inside the window: the ring cursor never
                // laps, so GPU slot k and CPU pool index k are the same particle and a
                // 1:1 comparison is meaningful.
                em.burst = kBurst;
                em.rate = 0.0f;
                em.loop = false;
                em.duration = 0.0f;
                em.maxParticles = 512;
                em.lifetime = 60.0f;
                em.lifetimeVariance = 0.35f;
                em.emitRadius = 0.6f;
                em.direction = {0.2f, 1.0f, -0.3f};
                em.startSpeed = 2.5f;
                em.speedVariance = 0.4f;
                em.spread = 0.45f;
                em.gravity = {0.3f, -1.6f, 0.1f};
                em.drag = 0.7f;                 // exercises the exp() arm
                em.turbulence = 1.4f;           // -> CurlNoiseForce (3 cos/particle)
                em.turbulenceScale = 0.9f;
                em.spin = 1.7f;                 // -> SpawnInitRotation + RotationRate
                em.startColor = {1.0f, 0.8f, 0.3f, 1.0f};
                em.endColor = {0.9f, 0.15f, 0.05f, 0.0f};
                em.colorVariance = 0.3f;        // exercises VarianceScale on both paths
                em.startSize = 0.5f;
                em.endSize = 0.12f;
                em.sizeVariance = 0.25f;
                em.fadeIn = 0.1f;
                em.fadeOut = 0.4f;
                reg.emplace<hbe::ParticleEmitter>(v.entity, em);
                return;
            }
            if (v.frame < 3) return;

            hbe::ParticleEmitter* em = reg.try_get<hbe::ParticleEmitter>(v.entity);
            if (!em || !em->stack.valid) return;

            // Compile the reference from the SAME builder the live emitter used.
            if (!v.armed) {
                std::string errs;
                if (!hbe::particle::BuildGpuDesc(*em).modules.empty() &&
                    hbe::vfx::Compile(hbe::particle::BuildGpuDesc(*em), v.refStack, &errs)) {
                    hbe::vfx::ReservePool(v.refStack, v.refPool);
                    v.refState.Reset(em->gpuSeed);
                    v.refState.emitting = true;
                    v.armed = true;
                } else {
                    v.why = "reference stack failed to compile";
                    v.done = true;
                    e.Quit();
                    return;
                }
            }

            // Lockstep the reference to the GPU emitter's frame count. emitterTime is
            // advanced by exactly dt per StepGpuEmitter call, so it IS the count.
            const hbe::u32 gpuFrames =
                static_cast<hbe::u32>(std::lround(em->state.emitterTime / kDt));
            while (v.refFrames < gpuFrames && v.refFrames < kSimFrames) {
                // Take this frame's operands from the live emitter rather than
                // re-deriving them - the mapping is under test, not duplicated.
                for (hbe::u32 st = 0; st < hbe::vfx::kStageCount; ++st) {
                    auto& live = em->stack.stages[st];
                    auto& ref = v.refStack.stages[st];
                    if (live.size() != ref.size()) continue;
                    for (size_t k = 0; k < live.size(); ++k) ref[k].params = live[k].params;
                }
                v.refStack.seed = em->stack.seed;
                v.refStack.spawnRate = em->stack.spawnRate;
                v.refStack.burst = em->stack.burst;
                v.refStack.loop = em->stack.loop;
                v.refStack.duration = em->stack.duration;
                v.refState.emitting = true;
                hbe::vfx::RunFrame(v.refStack, v.refState, v.refPool, kDt);
                ++v.refFrames;
            }
            if (v.refFrames < kSimFrames) return;

            // ONE frame of settle, and the reason is worth stating: the editor's
            // onFrame callback runs AFTER particle::Update and GpuSim::Update but
            // BEFORE Renderer::RenderScene, and the compute queue is only executed
            // inside the device's BeginFrame (which RenderScene drives). So at the
            // moment the reference reaches frame N, the GPU has executed N-1 steps and
            // step N is still sitting in the software queue. Reading here would compare
            // frame N against frame N-1 and report a plausible-looking one-gravity-step
            // difference - which is exactly what it did before this wait existed.
            if (!v.settled) {
                v.settled = true;
                return;
            }

            // --- read the simulation buffer back and compare ---
            const hbe::u32 n = v.refPool.count;
            if (n != kBurst) {
                v.why = "reference pool did not hold the burst";
                v.done = true;
                e.Quit();
                return;
            }
            const hbe::u32 first = em->gpuSlotBase + hbe::rhi::kGpuParticleEmitterElements;
            std::vector<hbe::vfx::GpuParticle> got(first + n);
            if (!r.ReadGpuBuffer(e.GetGpuSim().Records(), got.data(),
                                 static_cast<hbe::u32>(got.size() *
                                                       sizeof(hbe::vfx::GpuParticle)))) {
                v.why = "ReadGpuBuffer failed";
                v.done = true;
                e.Quit();
                return;
            }

            // MIXED tolerance, not a bare relative one. Several of these quantities
            // legitimately cross zero (velocity.y under gravity, position around the
            // origin), and a pure relative error there reports 2.0 for a difference of
            // one ULP - it would measure how close to zero the value is, not how far
            // apart the two paths are. `d <= atol + rtol*|v|` is the standard fix; the
            // score below is the failure MARGIN, so 1.0 is exactly at tolerance.
            constexpr hbe::f32 kAtol = 2.0e-3f;
            constexpr hbe::f32 kRtol = 2.0e-3f;
            const auto score = [](hbe::f32 a, hbe::f32 b) {
                const hbe::f32 d = std::fabs(a - b);
                const hbe::f32 m = std::max(std::fabs(a), std::fabs(b));
                return d / (kAtol + kRtol * m);
            };
            hbe::f32 worst = 0.0f;
            hbe::f32 travel = 0.0f;
            hbe::f32 worstA = 0.0f, worstB = 0.0f;
            hbe::u32 failing = 0;
            const char* worstField = "";
            hbe::u32 worstIdx = 0;
            // Colour is compared too. It is not a formality: it is the ONLY thing that
            // exercises VfxSpawnInitColor, VfxColorOverLife, VarianceScale on the
            // colour salt, and the half4 pack/unpack - the test emitter sets a
            // start->end ramp, fadeIn/fadeOut and colorVariance specifically for it.
            // Without these four fields all of that shipped unverified.
            const bool hasColor = v.refPool.Has(hbe::vfx::Attr::Color);
            for (hbe::u32 i = 0; i < n; ++i) {
                const hbe::vfx::GpuParticle& g = got[first + i];
                const glm::vec3 cp = v.refPool.position[i];
                travel = std::max(travel, glm::length(cp - glm::vec3(3.5f, 1.25f, -2.0f)));
                // The GPU record stores colour as half4; the CPU pool keeps float4.
                // Comparing zeros when the stream was eliminated keeps the field list
                // fixed-size without ever reading an unbacked stream.
                const glm::vec4 gc =
                    hasColor ? glm::vec4(glm::unpackHalf2x16(g.colorRG),
                                         glm::unpackHalf2x16(g.colorBA))
                             : glm::vec4(0.0f);
                const glm::vec4 cc = hasColor ? v.refPool.color[i] : glm::vec4(0.0f);
                struct { const char* name; hbe::f32 a, b; } fields[] = {
                    {"position.x", g.position.x, cp.x},
                    {"position.y", g.position.y, cp.y},
                    {"position.z", g.position.z, cp.z},
                    {"velocity.x", g.velocity.x, v.refPool.velocity[i].x},
                    {"velocity.y", g.velocity.y, v.refPool.velocity[i].y},
                    {"velocity.z", g.velocity.z, v.refPool.velocity[i].z},
                    {"age", g.age, v.refPool.age[i]},
                    {"lifetime", g.lifetime, v.refPool.lifetime[i]},
                    {"rotation", g.rotation, v.refPool.rotation[i]},
                    {"size", g.sizeX, v.refPool.sizeX[i]},
                    {"color.r", gc.r, cc.r},
                    {"color.g", gc.g, cc.g},
                    {"color.b", gc.b, cc.b},
                    {"color.a", gc.a, cc.a},
                };
                bool bad = false;
                for (const auto& f : fields) {
                    const hbe::f32 e2 = score(f.a, f.b);
                    if (e2 > 1.0f) bad = true;
                    if (e2 > worst) {
                        worst = e2;
                        worstField = f.name;
                        worstIdx = i;
                        worstA = f.a;
                        worstB = f.b;
                    }
                }
                if (bad) ++failing;
            }
            v.worstRel = worst;
            v.travelled = travel;
            v.compared = n;
            // The movement check is what stops a pair of zero pools from passing - a
            // parity test that both sides satisfy by doing nothing proves nothing.
            v.pass = (worst <= 1.0f) && (travel > 0.25f);
            v.why = v.pass ? "ok" : (travel <= 0.25f ? "particles never moved" : worstField);
            if (!v.pass) {
                std::printf("  %u/%u particles out of tolerance; worst '%s' on particle %u: "
                            "gpu %.7f vs cpu %.7f (margin %.2f, travel %.3f)\n",
                            failing, n, worstField, worstIdx, worstA, worstB, worst, travel);
            }
            v.done = true;
            e.Quit();
        });
        vsEngine.Run(config);
        std::printf("vfxsim %s (%s) - %u particles x %u frames, worst relative diff %.2e, "
                    "max travel %.2f m\n",
                    v.pass ? "PASS" : "FAIL", v.why, v.compared, kSimFrames, v.worstRel,
                    v.travelled);
        return v.pass ? 0 : 1;
    }

    // --render-movie <cutscene.hbcutscene | "current"> [--out FILE.mp4|DIR] [--res WxH]
    //   [--fps N] [--seconds S] [--music FILE]: offline-render a trailer, then exit. An
    //   .mp4 out encodes H.264 video + AAC audio (Media Foundation); a directory out
    //   writes a lossless PNG frame sequence. Reuses the editor's offscreen render + the
    //   deterministic cutscene evaluator at a fixed dt.
    std::string renderMovie;
    static hbe::movie::MovieConfig s_cfg;
    s_cfg.outputDir = "movie_frames";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--render-movie") == 0 && i + 1 < argc) renderMovie = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            const std::filesystem::path o = argv[++i];
            std::string ext = o.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".mp4") s_cfg.outputFile = o;
            else s_cfg.outputDir = o;
        }
        else if (std::strcmp(argv[i], "--res") == 0 && i + 1 < argc)
            std::sscanf(argv[++i], "%ux%u", &s_cfg.width, &s_cfg.height);
        else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc)
            s_cfg.fps = static_cast<hbe::u32>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            s_cfg.duration = static_cast<hbe::f32>(std::atof(argv[++i]));
        else if (std::strcmp(argv[i], "--music") == 0 && i + 1 < argc) s_cfg.musicRel = argv[++i];
    }
    if (!renderMovie.empty()) {
        if (!hbe::Project::HasActive()) {
            std::printf("--render-movie requires --project\n");
            return 1;
        }
        s_cfg.cutsceneRel = (renderMovie == "current") ? std::string() : renderMovie;
        static hbe::Editor mvEditor;
        static hbe::movie::MovieJob mvJob;
        static bool mvStarted = false;
        hbe::Engine mvEngine;
        mvEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
            e.GetScene().SetEditorView(true);
            if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd))
                hbe::Editor::ApplyTheme();
        });
        mvEngine.SetOnFrame([](hbe::Engine& e) {
            mvEditor.BuildUI(e); // proven offscreen + ImGui plumbing; job overrides the size
            if (!mvStarted) {
                mvStarted = true;
                mvJob.Start(e, hbe::Project::Active().AssetsDir(), s_cfg);
            }
            if (mvJob.Active()) {
                mvJob.Tick(e);
            } else {
                const auto target = s_cfg.outputFile.empty() ? s_cfg.outputDir : s_cfg.outputFile;
                std::printf("render-movie done: %u/%u frames -> %s\n", mvJob.FramesWritten(),
                            mvJob.TotalFrames(), target.string().c_str());
                // Headless verify: decode frame 0 of the .mp4 back to a PNG.
                if (!s_cfg.outputFile.empty()) {
                    auto verify = s_cfg.outputFile;
                    verify.replace_extension(".verify.png");
                    const bool vok = hbe::movie::DecodeFirstFrameToPng(s_cfg.outputFile, verify);
                    std::printf("verify-decode %s -> %s\n", vok ? "OK" : "FAIL",
                                verify.string().c_str());
                }
                mvJob.Stop(e);
                e.Quit();
            }
        });
        return mvEngine.Run(config);
    }

    hbe::Engine engine;
    hbe::Editor editor;

    // Initialize ImGui and route Win32 input to it. Physics starts paused in
    // the editor (toggled via the Stats panel's "Simulate physics").
    engine.SetOnInit([](hbe::Engine& e) {
        e.GetPhysics().SetRunning(false); // the Game tab's Play starts the sim
        e.SetGameCameraEnabled(false);    // scene view owns the camera until Play
        e.GetScene().SetEditorView(true); // honor per-entity EditorHidden in the viewport
        // Seeded here as well as every frame in Editor::BuildUI: the frame loop runs
        // ui::UpdateAnimations BEFORE onFrame_, so without this the very first editor
        // frame would advance a document's clips and bake one animated pose into the
        // authored offset/scale/colour of every element carrying one.
        e.GetScene().SetUIAuthoringView(true);
        if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd)) {
            hbe::Editor::ApplyTheme();
            hbe::Editor::EnableLayoutPersistence("HeartbreakEditor.ini"); // save/restore docking
            e.GetWindow().SetWndProcHook([](void* h, hbe::u32 m, hbe::u64 w, hbe::i64 l) -> hbe::i64 {
                return ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(h), m,
                                                      static_cast<WPARAM>(w),
                                                      static_cast<LPARAM>(l));
            });
        }
    });

    // Build the editor UI each frame.
    engine.SetOnFrame([&editor](hbe::Engine& e) { editor.BuildUI(e); });

    return engine.Run(config);
}
