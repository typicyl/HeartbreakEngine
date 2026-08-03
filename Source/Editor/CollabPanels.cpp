// Editor/CollabPanels.cpp - the front door for collaboration.
//
// Its own translation unit, following UIEditor.cpp / DialogueEditor.cpp: Editor.cpp is
// already ~18k lines and on /bigobj.
//
// WHY THERE ARE NO TEXT BOXES FOR THE INVITATION. An invitation carries a whole WebRTC
// description and is a couple of thousand characters. Every text field in this editor is
// a fixed `char buf[N]` that silently truncates, and a truncated invitation still
// base64-decodes into a plausible-looking description - so it would produce a connection
// that hangs with no diagnosis, which is the single worst outcome for a copy-paste
// workflow. Clipboard buttons move the whole string or nothing.
#include "Editor/Editor.h"

#include "Core/Log.h"
#include "Engine/Engine.h"
#include "Project/Project.h"
#include "Scene/Scene.h"

#include <imgui.h>

#include <cstdio>

namespace hbe {

namespace {

// The recurring inline palette - there is no badge helper in this codebase.
const ImVec4 kGood(0.45f, 0.85f, 0.5f, 1.0f);
const ImVec4 kWarn(1.0f, 0.78f, 0.35f, 1.0f);
const ImVec4 kBad(1.0f, 0.4f, 0.4f, 1.0f);

void CopyButton(const char* label, const std::string& text) {
    ImGui::BeginDisabled(text.empty());
    if (ImGui::Button(label)) ImGui::SetClipboardText(text.c_str());
    ImGui::EndDisabled();
}

// A fingerprint is meant to be read out loud, so give it room and let it be copied.
void Fingerprint(const char* label, const std::string& fp) {
    ImGui::Text("%s", label);
    ImGui::SameLine();
    ImGui::TextColored(kWarn, "%s", fp.c_str());
}

} // namespace

void Editor::DrawCollaborate(Engine& engine) {
    if (!panelOpen_[Panel_Collaborate]) return;
    if (!ImGui::Begin("Collaborate", &panelOpen_[Panel_Collaborate])) {
        ImGui::End();
        return;
    }
    // Claim BEFORE any early return. Begin() pushes the focus scope unconditionally, so
    // without this a Ctrl+V aimed at this panel falls through and pastes the scene
    // clipboard - entities - into the level.
    ClaimFocus(editor::SaveSurface::Collaborate);

    editor::CollabSession& s = collab_;
    if (!s.HasIdentity()) s.EnsureIdentity();

    // NO PROJECT IS NOT AN ERROR HERE, and gating this panel on one was a real
    // chicken-and-egg: JOINING is how a person WITHOUT a project gets one, so refusing to
    // draw the panel until they had opened something left them with a working feature
    // they could not reach. HOSTING needs a project (you are sharing yours); joining and
    // downloading do not.
    const bool haveProject = Project::HasActive();
    if (haveProject) s.SetProjectRoot(Project::Active().Root());

    Fingerprint("This machine:", s.MyFingerprint());
    ImGui::SameLine();
    CopyButton("Copy##fp", s.MyFingerprint());
    ImGui::SameLine();
    // THE FULL KEY, not the fingerprint. The fingerprint is 8 bytes of a hash - a human
    // check, not something that can authorise anyone - so pre-authorising a colleague
    // BEFORE they have ever connected needs the whole thing.
    if (ImGui::Button("Copy my key"))
        ImGui::SetClipboardText(collab::KeyToHex(s.MyKey()).c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Send this to the host so they can add you before you connect.");
    ImGui::TextDisabled("%s", "Read the fingerprint out to whoever is hosting so they can "
                              "check it; send them the key so they can add you.");
    ImGui::Separator();

    ImGui::TextColored(s.Live() ? kGood : ImGui::GetStyle().Colors[ImGuiCol_Text], "%s",
                       s.Status().c_str());
    ImGui::Separator();

    switch (s.Role()) {
    case editor::SessionRole::Offline: {
        ImGui::BeginDisabled(!haveProject);
        if (ImGui::Button("Host a session")) s.StartHosting();
        ImGui::EndDisabled();
        if (!haveProject && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Open the project you want to share first.");
        ImGui::SameLine();
        if (ImGui::Button("Join with an invitation")) {
            if (const char* t = ImGui::GetClipboardText()) {
                if (!s.PrepareJoin(t)) { /* Status() explains */
                }
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Copy the invitation they sent you, then click this.");
        break;
    }
    case editor::SessionRole::Hosting: {
        const collab::LinkState st = s.InvitationState();
        if (s.Invitation().empty()) {
            if (st == collab::LinkState::Closed) {
                if (ImGui::Button("Create an invitation")) s.CreateInvitation();
            } else {
                ImGui::TextDisabled("Preparing an invitation... (%s)",
                                    collab::LinkStateName(st));
            }
        } else {
            CopyButton("Copy the invitation", s.Invitation());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", "send it to them however you like");
            if (ImGui::Button("Paste their reply")) {
                if (const char* t = ImGui::GetClipboardText()) s.AcceptReply(t);
            }
            // THE RAW LINK STATE, always visible. "Connecting..." on its own is not a
            // diagnosis - this says whether it is still finding addresses, waiting on the
            // far side, actually linking, or already dead.
            ImGui::TextDisabled("Link: %s", collab::LinkStateName(st));
            if (st == collab::LinkState::Closed || st == collab::LinkState::Failed)
                ImGui::TextColored(kBad, "%s",
                                   "This invitation is finished. Create a new one.");
        }
        ImGui::Separator();
        ImGui::Text("People connected: %d", static_cast<int>(s.PeerCount()));
        // Sharing the FILES, as opposed to the live edits. Someone joining with an empty
        // folder needs this or they have nothing to edit.
        if (s.SharedFileCount() == 0) {
            // The host is the only one who can fix this, and the guest's screen claims the
            // copy succeeded - so this is the only place the truth can appear at all.
            if (s.AsksWithNothingToSend() > 0)
                ImGui::TextColored(kBad,
                                   "%d request(s) for this project got nothing back - "
                                   "share it and ask them to try again.",
                                   static_cast<int>(s.AsksWithNothingToSend()));
            else if (s.PeerCount() > 0)
                ImGui::TextColored(kWarn, "%s",
                                   "Nobody can copy this project until you share it.");
            if (ImGui::Button("Share the project files")) s.ShareProject();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Lets someone who has nothing copy the whole project. "
                                  "Build output and your people list are never sent.");
        } else {
            ImGui::TextColored(kGood, "Sharing %d file(s)",
                               static_cast<int>(s.SharedFileCount()));
            ImGui::SameLine();
            if (ImGui::Button("Rescan")) s.ShareProject();
        }
        if (ImGui::Button("Stop hosting")) s.Leave();
        break;
    }
    case editor::SessionRole::Confirming: {
        ImGui::TextWrapped("%s", "This invitation says it is from:");
        ImGui::TextColored(kWarn, "%s", s.PendingHostFingerprint().c_str());
        ImGui::TextWrapped(
            "%s", "Check that against what they told you. If it does not match, someone "
                  "else sent you this.");
        if (ImGui::Button("Connect")) s.ConfirmJoin();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) s.CancelJoin();
        break;
    }
    case editor::SessionRole::Joining: {
        ImGui::TextDisabled("Link: %s", collab::LinkStateName(s.GuestLinkState()));
        if (s.GuestLinkError() && *s.GuestLinkError())
            ImGui::TextColored(kBad, "%s", s.GuestLinkError());
        if (s.Reply().empty()) {
            ImGui::TextDisabled("%s", "Preparing a reply...");
        } else {
            CopyButton("Copy the reply", s.Reply());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", "send this back to them");
        }
        if (ImGui::Button("Cancel")) s.Leave();
        break;
    }
    case editor::SessionRole::Joined: {
        ImGui::TextColored(kGood, "%s", "Connected.");
        ImGui::Separator();
        // STARTING FROM NOTHING. The point of this button is the person who has the
        // engine and an empty folder.
        switch (s.Download()) {
        case editor::CollabSession::DownloadPhase::Idle:
        case editor::CollabSession::DownloadPhase::Done:
        case editor::CollabSession::DownloadPhase::Failed: {
            ImGui::TextWrapped("%s", "Copy their whole project into a folder on this "
                                     "machine. Paste the folder to put it in:");
            ImGui::SetNextItemWidth(380.0f);
            ImGui::InputTextWithHint("##dlpath", "C:\\Projects\\TheirGame",
                                     collabDownloadPath_, sizeof(collabDownloadPath_));
            ImGui::SameLine();
            if (ImGui::Button("Paste##dl")) {
                if (const char* t = ImGui::GetClipboardText())
                    std::snprintf(collabDownloadPath_, sizeof(collabDownloadPath_), "%s", t);
            }
            const bool has = collabDownloadPath_[0] != '\0';
            ImGui::BeginDisabled(!has);
            if (ImGui::Button("Get the project")) s.StartDownload(collabDownloadPath_);
            ImGui::EndDisabled();
            if (!has && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Choose where to put it first.");
            if (s.Download() == editor::CollabSession::DownloadPhase::Failed)
                ImGui::TextColored(kBad, "%s", s.DownloadError().c_str());
            else if (s.Download() == editor::CollabSession::DownloadPhase::Done) {
                ImGui::TextColored(kGood, "%s", "Copied.");
                // OPEN IT FROM HERE. Telling someone who just arrived with nothing to go
                // and find the project themselves is the step where this stops feeling
                // like it worked - and they may not have a project open at all, which is
                // exactly the state this whole flow exists to get them out of.
                ImGui::SameLine();
                if (ImGui::Button("Open this project")) {
                    const std::filesystem::path root = s.DownloadRoot();
                    std::error_code ec;
                    std::filesystem::path hbproj;
                    for (const auto& de : std::filesystem::directory_iterator(root, ec))
                        if (de.path().extension() == ".hbproj") { hbproj = de.path(); break; }
                    if (hbproj.empty()) {
                        SetSaveStatus("That folder has no .hbproj in it - the sender may "
                                      "not have shared a whole project.",
                                      true);
                    } else if (Project::Active().Open(hbproj)) {
                        OnProjectChanged();
                        SetSaveStatus("Opened " + hbproj.filename().string() + ".", false);
                    } else {
                        SetSaveStatus("Could not open that project.", true);
                    }
                }
            }
            break;
        }
        default:
            ImGui::Text("%s: %d/%d file(s), %.1f MB",
                        editor::CollabSession::DownloadPhaseName(s.Download()),
                        static_cast<int>(s.DownloadFilesDone()),
                        static_cast<int>(s.DownloadFilesTotal()),
                        static_cast<double>(s.DownloadBytes()) / (1024.0 * 1024.0));
            break;
        }
        ImGui::Separator();
        if (ImGui::Button("Leave")) s.Leave();
        break;
    }
    }

    // --- history ---------------------------------------------------------
    ImGui::Separator();
    ImGui::TextDisabled("%s", "History");
    Scene& scene = engine.GetScene();
    if (currentScenePath_.empty()) {
        ImGui::TextDisabled("%s", "Save this scene once before it can have history.");
    } else if (!editor::CollabSession::SceneHasHistory(scene)) {
        ImGui::TextWrapped("%s",
                           "This scene has no history yet. Turning it on stamps a "
                           "permanent id into the scene file, so it has to be saved "
                           "afterwards.");
        if (ImGui::Button("Turn on history for this scene")) {
            if (editor::CollabSession::EnableHistory(scene)) {
                SetSaveStatus("History is on for this scene - save it to write the id.",
                              false);
            } else {
                SetSaveStatus("Could not turn on history for this scene.", true);
            }
        }
    } else {
        ImGui::Text("%d change set(s) recorded.",
                    static_cast<int>(s.History().Commits().size()));
        ImGui::TextDisabled(
            "%s", editor::JournalFile(currentScenePath_).filename().string().c_str());
        if (ImGui::Button("Review someone else's changes...")) wantCollabImport_ = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Paste the path of the .hbjournal file they sent you.");
    }

    // A popup cannot be opened from inside another widget's handler in every case, so
    // this follows the editor's want*-flag idiom - and the IsPopupOpen guard stops it
    // being re-opened every frame while the flag is set.
    if (wantCollabImport_ && !ImGui::IsPopupOpen("Review changes from a file")) {
        collabImportPath_[0] = '\0';
        ImGui::OpenPopup("Review changes from a file");
    }
    if (ImGui::BeginPopupModal("Review changes from a file", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s",
                           "A collaborator's history file sits next to their scene and "
                           "ends in .hbjournal. Paste its full path here.");
        ImGui::SetNextItemWidth(460.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter =
            ImGui::InputTextWithHint("##jpath", "C:\\...\\Level.hbscene.hbjournal",
                                     collabImportPath_, sizeof(collabImportPath_),
                                     ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Paste")) {
            if (const char* t = ImGui::GetClipboardText())
                std::snprintf(collabImportPath_, sizeof(collabImportPath_), "%s", t);
        }
        const bool has = collabImportPath_[0] != '\0';
        ImGui::BeginDisabled(!has);
        if (ImGui::Button("Compare") || (enter && has)) {
            collab::Journal theirs;
            bool truncated = false;
            if (!theirs.Load(collabImportPath_, &truncated)) {
                SetSaveStatus("Could not read that history file.", true);
            } else if (!s.BeginReview(theirs, scene)) {
                SetSaveStatus("This scene has no history to compare against.", true);
            } else {
                panelOpen_[Panel_Review] = true; // there is something to decide now
                if (truncated)
                    SetSaveStatus("Their history file ended mid-entry; the last change "
                                  "set was ignored.",
                                  true);
            }
            wantCollabImport_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            wantCollabImport_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup(); // inside the if, unlike EndChild - see the house pattern
    }

    ImGui::End();
}

void Editor::DrawCollabPeople(Engine&) {
    if (!panelOpen_[Panel_People]) return;
    if (!ImGui::Begin("People", &panelOpen_[Panel_People])) {
        ImGui::End();
        return;
    }
    ClaimFocus(editor::SaveSurface::Collaborate);

    editor::CollabSession& s = collab_;
    if (!Project::HasActive()) {
        ImGui::TextColored(kWarn, "%s", "Open a project first.");
        ImGui::End();
        return;
    }
    s.SetProjectRoot(Project::Active().Root());

    ImGui::TextWrapped("%s",
                       "Only the people listed here can join this project. The list "
                       "starts empty, which means nobody.");
    ImGui::Separator();

    // ADD SOMEONE WHO HAS NEVER CONNECTED.
    //
    // Without this the ONLY way in was the knock below - they had to try, be refused, and
    // be admitted afterwards. That is fine once a link works, and useless when it does
    // not: if the connection never reaches the handshake there is no knock, and no way to
    // authorise anyone at all.
    ImGui::TextDisabled("%s", "Add someone by their key (they click 'Copy my key'):");
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputTextWithHint("##newname", "their name", collabLabelBuf_,
                             sizeof(collabLabelBuf_));
    ImGui::SameLine();
    const bool namedNew = collabLabelBuf_[0] != 0;
    ImGui::BeginDisabled(!namedNew);
    if (ImGui::Button("Paste their key")) {
        collab::PublicKey k{};
        const char* t = ImGui::GetClipboardText();
        if (!t || !collab::KeyFromHex(t, k)) {
            SetSaveStatus("That is not a key. It is 128 characters - a fingerprint like "
                          "a1b2:c3d4 is not enough to add someone.",
                          true);
        } else if (s.AllowPerson(k, collabLabelBuf_)) {
            SetSaveStatus(std::string("Added ") + collabLabelBuf_ + " (" +
                              collab::Fingerprint(k) + ").",
                          false);
            collabLabelBuf_[0] = 0;
        }
    }
    ImGui::EndDisabled();
    if (!namedNew && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Give them a name first.");
    ImGui::Separator();

    // Anyone who tried and was turned away. This IS the invite flow: a host who cannot
    // see who knocked has no way to let anybody in.
    if (!s.Knocks().empty()) {
        ImGui::TextColored(kWarn, "%s", "Someone tried to join:");
        for (usize i = 0; i < s.Knocks().size(); ++i) {
            const editor::CollabSession::Knock& k = s.Knocks()[i];
            ImGui::PushID(static_cast<int>(i)); // two rows share button labels otherwise
            ImGui::TextColored(kWarn, "%s", k.fingerprint.c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::InputTextWithHint("##label", "their name", collabLabelBuf_,
                                     sizeof(collabLabelBuf_));
            ImGui::SameLine();
            const bool named = collabLabelBuf_[0] != '\0';
            ImGui::BeginDisabled(!named);
            if (ImGui::Button("Let them in")) {
                if (s.AllowPerson(k.key, collabLabelBuf_)) {
                    SetSaveStatus(std::string("Added ") + collabLabelBuf_ + " to this project.",
                                  false);
                    collabLabelBuf_[0] = '\0';
                }
                ImGui::EndDisabled();
                ImGui::PopID();
                break; // the list just changed underneath us
            }
            ImGui::EndDisabled();
            if (!named && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Give them a name first, so you can recognise them later.");
            ImGui::PopID();
        }
        ImGui::Separator();
    }

    if (s.People().empty()) {
        ImGui::TextDisabled("%s", "Nobody yet.");
    } else {
        for (usize i = 0; i < s.People().size(); ++i) {
            const collab::Allowlist::Entry& e = s.People()[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%s", e.label.empty() ? "(unnamed)" : e.label.c_str());
            ImGui::SameLine(180.0f);
            ImGui::TextDisabled("%s", collab::Fingerprint(e.key).c_str());
            ImGui::SameLine();
            if (ImGui::Button("Remove")) {
                s.RemovePerson(e.key);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void Editor::DrawCollabReview(Engine& engine) {
    if (!panelOpen_[Panel_Review]) return;
    if (!ImGui::Begin("Review changes", &panelOpen_[Panel_Review])) {
        ImGui::End();
        return;
    }
    ClaimFocus(editor::SaveSurface::Collaborate);

    editor::CollabSession& s = collab_;
    Scene& scene = engine.GetScene();

    if (!s.HasReview()) {
        ImGui::TextWrapped("%s",
                           "Nothing to review. Open a collaborator's history file from "
                           "the Collaborate panel to compare it with yours.");
        ImGui::End();
        return;
    }

    const collab::MergePlan& plan = s.ReviewPlan();
    ImGui::Text("Verdict: %s", collab::MergeVerdictName(plan.verdict));
    if (!plan.explanation.empty()) ImGui::TextWrapped("%s", plan.explanation.c_str());
    ImGui::Separator();

    if (plan.verdict == collab::MergeVerdict::NeedsReview) {
        ImGui::TextColored(kWarn, "%s",
                           "You both changed the same things. Nothing is applied until "
                           "every one of these is answered.");
        std::vector<scene::Resolution>& decisions = s.Decisions();
        ImGui::BeginChild("##conflicts", ImVec2(0.0f, -60.0f));
        for (usize i = 0; i < plan.conflicts.size() && i < decisions.size(); ++i) {
            const collab::Conflict& c = plan.conflicts[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Separator();
            ImGui::Text("Object %llu - %s", static_cast<unsigned long long>(c.guid),
                        c.component.c_str());
            // "%s" always: a value containing a '%' would otherwise be read as a format.
            ImGui::TextDisabled("was:    %s", "");
            ImGui::SameLine();
            ImGui::TextWrapped("%s", c.base.c_str());
            ImGui::Text("%s", "yours:");
            ImGui::SameLine();
            ImGui::TextWrapped("%s", c.mine.c_str());
            ImGui::Text("theirs (%s):", c.theirsAuthor.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", c.theirs.c_str());

            int choice = static_cast<int>(decisions[i]);
            if (ImGui::RadioButton("keep mine", choice == static_cast<int>(scene::Resolution::KeepMine)))
                decisions[i] = scene::Resolution::KeepMine;
            ImGui::SameLine();
            if (ImGui::RadioButton("take theirs",
                                   choice == static_cast<int>(scene::Resolution::TakeTheirs)))
                decisions[i] = scene::Resolution::TakeTheirs;
            if (decisions[i] == scene::Resolution::Undecided) {
                ImGui::SameLine();
                ImGui::TextColored(kBad, "%s", "undecided");
            }
            ImGui::PopID();
        }
        ImGui::EndChild(); // unconditional, matching the house pattern

        const bool ready = s.AllDecided();
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Apply")) {
            PushUndo(engine); // a merge is a scene edit; it must be undoable like any other
            std::string why;
            const usize n = s.ApplyReview(scene, &why);
            SetSaveStatus(why.empty()
                              ? "Applied " + std::to_string(n) + " change(s)."
                              : "Applied " + std::to_string(n) + " change(s) - " + why,
                          !why.empty());
        }
        ImGui::EndDisabled();
        if (!ready && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Answer every question above first.");
    } else if (plan.verdict == collab::MergeVerdict::RefusedEpoch ||
               plan.verdict == collab::MergeVerdict::RefusedDocument) {
        ImGui::TextColored(kBad, "%s", "These two copies cannot be merged.");
    } else if (plan.toApply.empty()) {
        ImGui::TextColored(kGood, "%s", "You already have everything they have.");
    } else {
        ImGui::Text("%d change set(s) to bring in.", static_cast<int>(plan.toApply.size()));
        if (ImGui::Button("Apply")) {
            PushUndo(engine);
            std::string why;
            const usize n = s.ApplyReview(scene, &why);
            SetSaveStatus(why.empty()
                              ? "Applied " + std::to_string(n) + " change(s)."
                              : "Applied " + std::to_string(n) + " change(s) - " + why,
                          !why.empty());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) s.DismissReview();

    ImGui::End();
}

// The SAME rule SaveSceneToDisk writes the active document with (`activeOnly`). Anything
// else would put entities from the additively-streamed scenes into this document's change
// set, and the far side would apply another file's edits to this one.
//
// Stateless by construction: it is stored on the session across frames, and a lambda that
// captured a `registry&` would dangle the first time a scene was replaced.
static bool ActiveDocumentOnly(const entt::registry& reg, entt::entity e) {
    return !reg.all_of<SceneSource>(e);
}

void Editor::CollabNoteOpened(Scene& scene, const std::filesystem::path& path) {
    // No identity is created here. A keypair is a per-user secret and generating one as a
    // side effect of opening a level would be a surprise; it is minted the first time the
    // author actually asks for collaboration.
    collab_.NoteSceneOpened(scene, path, &ActiveDocumentOnly);
}

void Editor::CollabNoteSaved(Scene& scene, const std::filesystem::path& writtenPath) {
    // OPT-IN. A scene with no docId has no history, and this does nothing at all - which
    // is what keeps every existing project, and every headless save in the self-tests,
    // exactly as it was.
    if (!editor::CollabSession::SceneHasHistory(scene)) return;
    if (!collab_.EnsureIdentity()) return;

    // Save As, or the first save of a scene that had no path: the history file follows
    // the file that was actually written.
    if (collab_.TrackedPath() != writtenPath)
        collab_.RetargetScene(scene, writtenPath, &ActiveDocumentOnly);

    std::string why;
    const usize n = collab_.SealCommit(scene, "saved scene", &why);
    if (!why.empty()) {
        // Never silent. If history is on and a save did not record, the author has to be
        // told now, not when a merge later turns out to be missing their work.
        SetSaveStatus("History not recorded: " + why, true);
    } else if (n > 0) {
        HBE_INFO("Collab: sealed {} change(s) into '{}'.", n,
                 editor::JournalFile(writtenPath).filename().string());
    }
}

void Editor::CollabTick(Engine& engine) {
    // Once per frame, in the same slot as the other real subsystems, BEFORE any panel
    // reads the world - a poll that lands between panel draws would give half the frame
    // one state and half another.
    if (collab_.Role() == editor::SessionRole::Offline && !collab_.TrackingScene()) return;
    const u64 now = static_cast<u64>(ImGui::GetTime() * 1000.0);
    // ORDER MATTERS. LiveSync drains what arrived last frame and applies it to the
    // registry, THEN broadcasts what the author changed. Running it after Tick would
    // apply this frame's arrivals a frame late and, worse, would broadcast the author's
    // change before folding in the remote one it was based on.
    collab_.LiveSync(engine.GetScene(), selected_, now);
    collab_.Tick(now);
}

} // namespace hbe
