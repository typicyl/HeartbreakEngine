// Editor/PasteParentTest.cpp - `--test-pasteparent`: THE PASTE-PARENTING CONTRACT.
//
// THE CONTRACT, stated once:
//
//   A pasted subtree lands where a human would expect it to: a copy of a CHILD
//   becomes a SIBLING of that child, under the same parent, last in the group. A
//   copy of a ROOT stays a root. Nothing else invents a parent.
//
// WHY THIS NEEDS A TEST AT ALL, i.e. why the bug was invisible. The clipboard
// fragment is COMPLETE - it round-trips every component of every entity in the
// subtree, and --test-pasteorder already proves it reproduces the source's child
// order at every depth. The one thing it cannot carry is the root's own parent,
// because EntityToJson only writes a `parent` key when the parent is inside the
// fragment (Scene/SceneSerializer.cpp) and the root's is BY DEFINITION outside it.
// So every fragment-level check passes and the paste still lands at the scene root:
// "I copy and paste and it becomes its own thing". The missing information is
// editor-session state, so it is captured in CopySelection and asserted here.
//
// The two properties that carry the most weight, and why each needs its own
// section:
//
//   * SECTION 1 IS THE REPORTED BUG, and it is written so it fails against the old
//     behaviour rather than describing it: the clone must carry a Parent, that
//     Parent must be the SOURCE's parent, and the clone must sort LAST among that
//     parent's children. Deleting the AttachPastedRoot call from PasteSubtree - the
//     exact status quo ante - fails all three.
//
//   * SECTION 5 IS THE HANDLE-STALENESS FAMILY, which is where a naive fix goes
//     wrong. An entity handle only means anything inside ONE registry: an undo, a
//     scene load or a Play -> Stop Replaces the world and entt hands the same
//     indices back out, so a remembered parent can be `valid()` and point at a
//     COMPLETELY different object. reg.valid() cannot see that; only the world
//     token can, which is why the token is captured alongside the handle. The
//     other two members of the family - destroyed-since, and a handle that now
//     aliases an entity THIS paste just created - are checked next to it.
//
// Headless: no GPU (a device-less Renderer uploads nothing and these entities
// reference no assets), no window, no ImGui context, no project. Same contract as
// --test-pasteorder, which owns the ORDER half of the same story.

#include "Editor/Editor.h"

#include "Core/Log.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Hierarchy.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "UI/UIDocument.h"

#include <string>
#include <vector>

namespace hbe {
namespace {

// The result of one modelled paste (see the ModelPaste lambda inside the test -
// it lives there rather than here because it drives Editor's private members).
struct PasteResult {
    entt::entity root = entt::null;
    std::vector<entt::entity> created;
};

// A named entity with a Transform, optionally under `parent`.
entt::entity Make(Scene& s, const char* name, entt::entity parent = entt::null,
                  const glm::vec3& pos = glm::vec3(0.0f)) {
    const entt::entity e = s.CreateEntity(name);
    Transform t;
    t.position = pos;
    s.Registry().emplace<Transform>(e, t);
    if (parent != entt::null) s.Registry().emplace<Parent>(e, Parent{parent});
    return e;
}

const std::string& NameOf(const Scene& s, entt::entity e) {
    static const std::string kNone = "(unnamed)";
    const Name* n = s.Registry().try_get<Name>(e);
    return n ? n->value : kNone;
}

entt::entity ParentOf(const Scene& s, entt::entity e) {
    const Parent* p = s.Registry().try_get<Parent>(e);
    return p ? p->entity : entt::null;
}

} // namespace

bool Editor::PasteParentSelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            HBE_ERROR("pasteparent: FAILED - {}", what);
            ok = false;
        }
    };

    Renderer renderer; // device-less: uploads nothing, which is the headless contract

    // Editor::PasteSubtree minus the Engine wrapper. What is left out is exactly
    // what needs an Engine and is covered elsewhere: the undo push, StageAssets
    // (these entities reference no assets), the UI-fragment refusal
    // (--test-uieditor) and the document Track. What is KEPT is the real
    // Editor::AttachPastedRoot - the code under test - reached in the same order
    // the editor reaches it.
    const auto ModelPaste = [&renderer](Editor& ed, Scene& scene,
                                        const std::string& fragment,
                                        entt::entity parentTo,
                                        const glm::vec3* placeAt = nullptr) {
        PasteResult out;
        scene::SceneData data;
        if (!scene::ParseSceneString(fragment, data) || data.entities.empty()) return out;
        scene::StagedAssets staged;
        scene::Instantiate(scene, renderer, data, staged, scene::LoadMode::Additive,
                           &out.created);
        if (out.created.empty()) return out;
        out.root = out.created.front();
        if (placeAt) {
            if (Transform* t = scene.Registry().try_get<Transform>(out.root))
                t->position = *placeAt;
        }
        ed.AttachPastedRoot(scene, out.root, parentTo, out.created);
        ed.selected_ = out.root;
        return out;
    };

    // --- 1. THE REPORTED BUG: a copied CHILD pastes as a SIBLING ---------------
    {
        Scene scene;
        Editor ed;
        auto& reg = scene.Registry();
        const entt::entity rig = Make(scene, "Rig");
        const entt::entity a = Make(scene, "Arm", rig, glm::vec3(1.0f, 2.0f, 3.0f));
        const entt::entity b = Make(scene, "Leg", rig);
        const entt::entity grip = Make(scene, "Grip", a); // the subtree has depth
        (void)b;
        (void)grip;

        ed.selected_ = a;
        ed.CopySelection(scene);
        expect(!ed.clipboard_.empty(), "(1) the copy produced a fragment");
        expect(ed.clipboardParent_ == rig,
               "(1) CopySelection must capture the SOURCE's parent - the fragment "
               "cannot carry it, so nothing else can");
        expect(ed.clipboardWorld_ == scene.WorldToken(),
               "(1) the capture must record which world that handle belongs to");
        expect(ed.ClipboardParentFor(scene) == rig,
               "(1) an unchanged world hands the captured parent back");

        const u64 srcGuid = reg.all_of<Guid>(a) ? reg.get<Guid>(a).value : 0;
        const PasteResult p = ModelPaste(ed, scene, ed.clipboard_,
                                         ed.ClipboardParentFor(scene));
        expect(p.root != entt::null && p.root != a, "(1) the paste created a new root");
        if (p.root == entt::null) return false;

        // THE THREE ASSERTIONS THE OLD BEHAVIOUR FAILS.
        expect(reg.all_of<Parent>(p.root),
               "(1) THE REGRESSION: a pasted clone must not become a scene root of "
               "its own - it must carry a Parent");
        expect(ParentOf(scene, p.root) == rig,
               "(1) THE REGRESSION: the clone's parent must be the SOURCE's parent, "
               "i.e. the clone is a SIBLING of what was copied");
        const std::vector<entt::entity> kids = scene::ChildrenOf(reg, rig);
        expect(kids.size() == 3, "(1) the source's parent gained exactly one child");
        expect(!kids.empty() && kids.back() == p.root,
               "(1) the clone sorts LAST among its new siblings (the rule every "
               "other creation path uses), not into the middle of them");

        // Identity and placement are untouched by the parenting.
        expect(reg.all_of<Guid>(p.root) && reg.get<Guid>(p.root).value != srcGuid &&
                   reg.get<Guid>(p.root).value != 0,
               "(1) a clone still MINTS a fresh guid - parenting is not identity");
        const Transform* ct = reg.try_get<Transform>(p.root);
        expect(ct && ct->position == glm::vec3(1.0f, 2.0f, 3.0f),
               "(1) the clone keeps the source's LOCAL transform verbatim (no "
               "world-transform rebase: it already sits in this parent's space, so "
               "rebasing would teleport it by the parent's transform)");
        // The subtree itself still came across whole - the fragment half is
        // --test-pasteorder's job, but a parenting fix that dropped children would
        // be a strange way to pass this file.
        expect(scene::ChildrenOf(reg, p.root).size() == 1,
               "(1) the clone kept its own descendants");

        // A second paste is a second sibling, after the first.
        const PasteResult q = ModelPaste(ed, scene, ed.clipboard_,
                                         ed.ClipboardParentFor(scene));
        const std::vector<entt::entity> kids2 = scene::ChildrenOf(reg, rig);
        expect(kids2.size() == 4 && kids2.back() == q.root &&
                   kids2[kids2.size() - 2] == p.root,
               "(1) repeated pastes stack in order under the same parent");
    }

    // --- 2. A copied ROOT still pastes as a ROOT --------------------------------
    {
        Scene scene;
        Editor ed;
        auto& reg = scene.Registry();
        const entt::entity rig = Make(scene, "Rig");
        Make(scene, "Arm", rig);

        ed.selected_ = rig;
        ed.CopySelection(scene);
        expect(ed.clipboardParent_ == entt::null,
               "(2) copying a root captures NO parent");
        const PasteResult p = ModelPaste(ed, scene, ed.clipboard_,
                                         ed.ClipboardParentFor(scene));
        expect(p.root != entt::null && !reg.all_of<Parent>(p.root),
               "(2) a copied ROOT pastes as a root - the fix must not invent a "
               "parent for something that never had one");
        expect(scene::ChildrenOf(reg, p.root).size() == 1,
               "(2) ...with its subtree intact");
    }

    // --- 3. Ctrl+D reads the SELECTION's parent, not the clipboard's ------------
    {
        Scene scene;
        Editor ed;
        auto& reg = scene.Registry();
        const entt::entity rigA = Make(scene, "RigA");
        const entt::entity rigB = Make(scene, "RigB");
        const entt::entity a = Make(scene, "Arm", rigA);
        const entt::entity b = Make(scene, "Leg", rigB);

        ed.selected_ = a;
        ed.CopySelection(scene); // clipboardParent_ == rigA from here on

        // DuplicateSelection's capture, verbatim: the parent of whatever is
        // selected NOW. Ctrl+D never touches clipboard_, so reading
        // clipboardParent_ here would silently re-home the duplicate.
        ed.selected_ = b;
        const Parent* bp = reg.try_get<Parent>(b);
        const entt::entity dupParent = (bp && reg.valid(bp->entity)) ? bp->entity
                                                                     : entt::null;
        expect(dupParent == rigB, "(3) the duplicate reads the SELECTION's parent");
        const PasteResult p = ModelPaste(ed, scene,
                                         scene::SaveSubtreeToString(scene, b), dupParent);
        expect(ParentOf(scene, p.root) == rigB,
               "(3) Ctrl+D duplicates INTO the selection's parent, not into "
               "whatever an older Ctrl+C happened to leave behind");
        expect(ed.clipboardParent_ == rigA,
               "(3) ...and the clipboard is untouched by a duplicate");
    }

    // --- 4. A .hbprefab drop is a ROOT ------------------------------------------
    {
        Scene scene;
        Editor ed;
        auto& reg = scene.Registry();
        const entt::entity rig = Make(scene, "Rig");
        const entt::entity a = Make(scene, "Arm", rig);
        ed.selected_ = a;
        ed.CopySelection(scene); // a live clipboard parent that must NOT leak in

        // InstantiatePrefab passes entt::null explicitly: a prefab came from a FILE,
        // it has no source parent, and `at` already places it.
        const std::string frag = scene::SaveSubtreeToString(scene, a);
        const glm::vec3 drop(9.0f, 0.0f, -4.0f);
        const PasteResult p =
            ModelPaste(ed, scene, frag, entt::null, &drop);
        expect(p.root != entt::null && !reg.all_of<Parent>(p.root),
               "(4) a prefab dropped from the asset browser is a ROOT - the "
               "clipboard's parent must not leak into it");
        const Transform* t = reg.try_get<Transform>(p.root);
        expect(t && t->position == drop, "(4) placeAt still positions the drop");
    }

    // --- 5. EVERY WAY THE CAPTURED PARENT CAN GO BAD ----------------------------
    {
        // (a) destroyed between the copy and the paste.
        Scene scene;
        Editor ed;
        auto& reg = scene.Registry();
        const entt::entity rig = Make(scene, "Rig");
        const entt::entity a = Make(scene, "Arm", rig);
        ed.selected_ = a;
        ed.CopySelection(scene);
        const std::string frag = ed.clipboard_;
        ed.DestroyRecursive(scene, rig); // takes `a` with it
        expect(!reg.valid(rig), "(5a) the parent really is gone");
        const PasteResult p = ModelPaste(ed, scene, frag,
                                         ed.ClipboardParentFor(scene));
        expect(p.root != entt::null, "(5a) the paste still happens");
        expect(!reg.all_of<Parent>(p.root),
               "(5a) a parent deleted since the copy falls back to a ROOT - and in "
               "particular is never emplaced as a DANGLING Parent, which would "
               "serialize as a child of nothing");
    }
    {
        // (b) the world was REPLACED (undo/redo, a scene load, Play -> Stop). The
        // handle can be valid and mean something else entirely, so the token - not
        // reg.valid() - is what decides whether the HANDLE may be used. But a
        // dropped parent is the reported bug coming back for the most ordinary
        // sequence there is ("copy, press Play, press Stop, paste"), so the parent is
        // ALSO remembered as a Guid and re-resolved in the new world. Both halves are
        // pinned here: it must follow the object, and it must not follow an index.
        Scene scene;
        Editor ed;
        auto& reg = scene.Registry();
        const entt::entity rig = Make(scene, "Rig");
        const entt::entity a = Make(scene, "Arm", rig);
        ed.selected_ = a;
        ed.CopySelection(scene);
        expect(ed.ClipboardParentFor(scene) == rig, "(5b) trusted before the replace");
        const u64 parentGuid = reg.get<Guid>(rig).value;
        expect(ed.clipboardParentGuid_ == parentGuid,
               "(5b) the copy records the parent's stable identity, not only a handle");

        // What undo/redo and Play -> Stop actually do: destroy the world and rebuild
        // it from a snapshot. Snapshots go through BuildSceneJson, which WRITES guids,
        // so the parent comes back as the same object under a fresh handle.
        reg.clear();
        scene.BumpWorldToken(); // what scene::DestroyWorld does on every Replace
        const entt::entity rig2 = Make(scene, "Rig");
        reg.emplace_or_replace<Guid>(rig2, Guid{parentGuid});
        expect(ed.ClipboardParentFor(scene) == rig2,
               "(5b) after a Replace the parent is found again BY GUID - otherwise "
               "copy -> Play -> Stop -> paste drops it and the reported bug returns");

        // ...and the index is still not trusted. A world in which that guid is absent
        // hands back nothing, no matter which handles happen to be valid.
        reg.clear();
        scene.BumpWorldToken();
        const entt::entity other = Make(scene, "SomethingElse");
        expect(reg.valid(other), "(5b) the rebuilt world has entities");
        expect(ed.ClipboardParentFor(scene) == entt::null,
               "(5b) a clipboard parent whose GUID is not in the new world is dropped, "
               "never resolved to whatever entity inherited its index");
    }
    {
        // (c) the handle now aliases an entity THIS paste created. Only reachable
        // through recycling, so it is exercised directly on the guard.
        Scene scene;
        Editor ed;
        auto& reg = scene.Registry();
        const entt::entity rig = Make(scene, "Rig");
        const entt::entity a = Make(scene, "Arm", rig);
        Make(scene, "Grip", a);
        ed.selected_ = a;
        ed.CopySelection(scene);

        scene::SceneData data;
        scene::StagedAssets staged;
        std::vector<entt::entity> created;
        expect(scene::ParseSceneString(ed.clipboard_, data), "(5c) fragment parses");
        scene::Instantiate(scene, renderer, data, staged, scene::LoadMode::Additive,
                           &created);
        expect(created.size() == 2, "(5c) the clone subtree exists");
        if (created.size() == 2) {
            // Pointing at the paste's own root...
            expect(ed.AttachPastedRoot(scene, created[0], created[0], created) ==
                       entt::null,
                   "(5c) a parent that IS the pasted root is refused (self-parent)");
            expect(!reg.all_of<Parent>(created[0]), "(5c) ...and nothing was emplaced");
            // ...and at one of its descendants.
            expect(ed.AttachPastedRoot(scene, created[0], created[1], created) ==
                       entt::null,
                   "(5c) a parent INSIDE the pasted subtree is refused (a cycle)");
            expect(!reg.all_of<Parent>(created[0]), "(5c) ...and nothing was emplaced");
            // A legitimate parent still attaches, so the guard is not a blanket no.
            expect(ed.AttachPastedRoot(scene, created[0], rig, created) == rig,
                   "(5c) a parent outside the paste still attaches");
            expect(ParentOf(scene, created[0]) == rig, "(5c) ...and it took");
        }
    }

    // --- 6. Documents: same one attaches, a different one re-homes to a root -----
    {
        Scene scene;
        Editor ed;
        ui::DocumentSet docs;
        ui::DocData fresh;
        const ui::DocHandle d1 =
            docs.OpenFromData(scene, nullptr, fresh, "A.hbui", true, /*preload*/ false);
        const ui::DocHandle d2 =
            docs.OpenFromData(scene, nullptr, fresh, "B.hbui", true, /*preload*/ false);
        expect(d1 != 0 && d2 != 0 && d1 != d2, "(6) two documents opened");
        auto& reg = scene.Registry();
        const auto mk = [&](ui::DocHandle d, const char* name) {
            const entt::entity e = scene.CreateEntity(name);
            docs.Track(scene, d, e);
            reg.emplace<UIElement>(e);
            return e;
        };
        const entt::entity groupA = mk(d1, "GroupA");
        const entt::entity groupB = mk(d2, "GroupB");
        const entt::entity cloneInA = mk(d1, "CloneInA");
        const entt::entity cloneInB = mk(d2, "CloneInB");
        const entt::entity worldObj = Make(scene, "Crate");

        expect(ed.AttachPastedRoot(scene, cloneInA, groupA, {cloneInA}) == groupA,
               "(6) inside ONE document the source parent is honoured");
        expect(ParentOf(scene, cloneInA) == groupA, "(6) ...and it took");

        // Copy out of document A, paste into document B: the clone has already been
        // re-Tracked into B by the time this runs, so A's parent is across a FILE
        // boundary. Unlike a drag - which the author aimed, so Reparent refuses it
        // out loud - this parent is an inference, so it re-homes instead of
        // failing the paste.
        expect(ed.AttachPastedRoot(scene, cloneInB, groupA, {cloneInB}) == entt::null,
               "(6) a parent in a DIFFERENT .hbui is dropped");
        expect(!reg.all_of<Parent>(cloneInB),
               "(6) ...so the clone lands as a root of the document it was pasted "
               "into, rather than hanging across the document boundary");
        expect(reg.all_of<UIDocMember>(cloneInB) &&
                   reg.get<UIDocMember>(cloneInB).doc == d2,
               "(6) ...and keeps the membership the paste gave it");

        // The scene/document boundary is the same boundary.
        expect(ed.AttachPastedRoot(scene, worldObj, groupB, {worldObj}) == entt::null,
               "(6) scene content is never parented under document content");
        expect(!reg.all_of<Parent>(worldObj), "(6) ...and nothing was emplaced");
        expect(ed.AttachPastedRoot(scene, cloneInA, worldObj, {cloneInA}) == entt::null,
               "(6) ...nor document content under scene content");
    }

    // --- 7. Prefab REVERT restores the INSTANCE's own parent --------------------
    {
        Scene scene;
        Editor ed;
        auto& reg = scene.Registry();
        const entt::entity rig = Make(scene, "Rig");
        const entt::entity first = Make(scene, "First", rig);
        const entt::entity inst = Make(scene, "Instance", rig, glm::vec3(4.0f, 0.0f, 0.0f));
        Make(scene, "Barrel", inst);
        const entt::entity last = Make(scene, "Last", rig);
        (void)first;
        (void)last;
        // Something else is on the clipboard, from a DIFFERENT parent, and must not
        // reach the revert.
        ed.selected_ = last;
        ed.CopySelection(scene);
        const entt::entity otherRig = Make(scene, "OtherRig");
        reg.emplace_or_replace<Parent>(last, Parent{otherRig});
        ed.selected_ = last;
        ed.CopySelection(scene);
        expect(ed.clipboardParent_ == otherRig, "(7) the clipboard points elsewhere");

        // RevertPrefabInstance's sequence, verbatim: capture placement, parent, guid
        // and sibling order; destroy; paste with the CAPTURED parent; restore guid
        // and order afterwards (a revert must visibly change nothing).
        const std::string frag = scene::SaveSubtreeToString(scene, inst);
        const glm::vec3 place = reg.get<Transform>(inst).position;
        const bool hadParent = reg.all_of<Parent>(inst);
        const entt::entity keepParent = hadParent ? reg.get<Parent>(inst).entity
                                                  : entt::null;
        const u64 keepGuid = reg.get<Guid>(inst).value;
        const HierarchyOrder keepOrder = reg.get<HierarchyOrder>(inst);
        ed.DestroyRecursive(scene, inst);
        const PasteResult p = ModelPaste(ed, scene, frag,
                                         hadParent ? keepParent : entt::null, &place);
        expect(p.root != entt::null, "(7) the revert re-instantiated the subtree");
        if (p.root != entt::null) {
            reg.emplace_or_replace<Guid>(p.root, Guid{keepGuid});
            reg.emplace_or_replace<HierarchyOrder>(p.root, keepOrder);
            expect(ParentOf(scene, p.root) == rig,
                   "(7) revert restores the INSTANCE's own parent, never the "
                   "clipboard's");
            expect(reg.get<Guid>(p.root).value == keepGuid,
                   "(7) a revert keeps the instance's identity (it is a restore of "
                   "THIS object, not a copy of one)");
            const std::vector<entt::entity> kids = scene::ChildrenOf(reg, rig);
            expect(kids.size() == 2 && NameOf(scene, kids[0]) == "First" &&
                       NameOf(scene, kids[1]) == "Instance",
                   "(7) ...and its sibling position: the restored order wins over "
                   "the paste's fresh mint, so a revert does not make the object "
                   "jump to the bottom of the list");
            expect(reg.get<Transform>(p.root).position == place,
                   "(7) ...and its placement");
        }
    }

    // --- 8. The SceneSource half: a clone joins its new parent's FILE ------------
    {
        // A paste under a parent that belongs to a streamed scene must save into
        // that scene's file, or it silently teleports on the next load (the class
        // of bug Scene/StrokeZone.h describes). Reparent does this; so must a paste.
        Scene scene;
        Editor ed;
        auto& reg = scene.Registry();
        const entt::entity rig = Make(scene, "Rig");
        reg.emplace<SceneSource>(rig, SceneSource{"Scenes/Streamed.hbscene"});
        const entt::entity a = Make(scene, "Arm", rig);
        reg.emplace<SceneSource>(a, SceneSource{"Scenes/Streamed.hbscene"});
        Make(scene, "Grip", a);

        ed.selected_ = a;
        ed.CopySelection(scene);
        const PasteResult p = ModelPaste(ed, scene, ed.clipboard_,
                                         ed.ClipboardParentFor(scene));
        expect(p.root != entt::null, "(8) the paste happened");
        if (p.root != entt::null) {
            const SceneSource* ss = reg.try_get<SceneSource>(p.root);
            expect(ss && ss->scene == "Scenes/Streamed.hbscene",
                   "(8) the clone is tagged with its new parent's scene, so it "
                   "saves into the same FILE as the parent it hangs from");
            for (const entt::entity kid : scene::ChildrenOf(reg, p.root)) {
                const SceneSource* ks = reg.try_get<SceneSource>(kid);
                expect(ks && ks->scene == "Scenes/Streamed.hbscene",
                       "(8) ...and so is its whole subtree");
            }
        }
    }

    // --- 9. A DROPPED PARENT MUST NOT TELEPORT THE CLONE ------------------------
    {
        // The clone's stored TRS is the source's LOCAL transform, so accepting the
        // parent must not rebase it (section 1 covers that: the clone lands on top of
        // the original). The mirror case is this one: when the parent is REJECTED,
        // the same numbers are reinterpreted as world and the clone lands wherever
        // the parent's transform was subtracted - off-screen, as the new selection.
        Scene scene;
        Editor ed;
        auto& reg = scene.Registry();
        const entt::entity rig = Make(scene, "Rig", entt::null, glm::vec3(100.0f, 0.0f, 0.0f));
        const entt::entity lamp = Make(scene, "Lamp", rig); // local (0,0,0) = world (100,0,0)
        ed.selected_ = lamp;
        ed.CopySelection(scene);
        expect(ed.clipboardHasWorldMatrix_, "(9) the copy records where the source IS");
        const glm::mat4 world = ed.clipboardWorldMatrix_;
        expect(glm::vec3(world[3]) == glm::vec3(100.0f, 0.0f, 0.0f),
               "(9) ...which is the WORLD position, not the local one");
        const std::string frag = ed.clipboard_;
        ed.DestroyRecursive(scene, rig); // the parent dies between copy and paste

        scene::SceneData data;
        scene::StagedAssets staged;
        std::vector<entt::entity> created;
        expect(scene::ParseSceneString(frag, data), "(9) fragment parses");
        scene::Instantiate(scene, renderer, data, staged, scene::LoadMode::Additive, &created);
        expect(!created.empty(), "(9) the paste still happens");
        if (!created.empty()) {
            const entt::entity root = created.front();
            expect(ed.AttachPastedRoot(scene, root, ed.ClipboardParentFor(scene), created,
                                       &world) == entt::null,
                   "(9) the dead parent is still refused");
            const Transform* t = reg.try_get<Transform>(root);
            expect(t && t->position == glm::vec3(100.0f, 0.0f, 0.0f),
                   "(9) and the rootless clone keeps the WORLD placement the author "
                   "last saw - it does not jump to the parent-relative origin");
        }
        // The guard is for a REQUESTED parent only: a prefab drop asks for none and is
        // positioned by placeAt, so a world matrix must not be applied over it.
        std::vector<entt::entity> created2;
        scene::Instantiate(scene, renderer, data, staged, scene::LoadMode::Additive, &created2);
        if (!created2.empty()) {
            const entt::entity root2 = created2.front();
            if (Transform* t = reg.try_get<Transform>(root2)) t->position = glm::vec3(7.0f);
            ed.AttachPastedRoot(scene, root2, entt::null, created2, &world);
            const Transform* t2 = reg.try_get<Transform>(root2);
            expect(t2 && t2->position == glm::vec3(7.0f),
                   "(9) a paste that asked for NO parent is left exactly where it was "
                   "placed");
        }
    }

    if (ok)
        HBE_INFO("PasteParentSelfTest PASS: a copied child pastes as a sibling (last "
                 "in the group), a copied root stays a root, Ctrl+D reads the "
                 "selection, a prefab drop is a root, revert restores the instance's "
                 "own parent, and every stale/aliasing/cross-document parent falls "
                 "back safely.");
    return ok;
}

} // namespace hbe
