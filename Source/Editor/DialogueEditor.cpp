// Editor/DialogueEditor.cpp - branching-dialogue node-graph editor (its own window).
//
// Same Blueprints-style look/interaction as the Schematic editor (draggable nodes,
// bezier wires, right-click add-node menu, pan), but authors a dlg::Graph: Start,
// Line, Choice, Condition, SetFlag, End. A node inspector on the right edits the
// selected node's fields. Kept in its own translation unit so Editor.cpp doesn't
// grow further.
#include "Editor/Editor.h"

#include "Core/Log.h"
#include "Dialogue/DialogueGraph.h"
#include "Project/Project.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace hbe {

namespace {
// Title-bar colour per node type (mirrors the schematic category palette).
ImU32 DlgNodeColor(dlg::NodeType t) {
    switch (t) {
        case dlg::NodeType::Start:     return IM_COL32(60, 110, 66, 255);
        case dlg::NodeType::Line:      return IM_COL32(58, 70, 104, 255);
        case dlg::NodeType::Choice:    return IM_COL32(92, 62, 112, 255);
        case dlg::NodeType::Condition: return IM_COL32(104, 88, 52, 255);
        case dlg::NodeType::SetFlag:   return IM_COL32(52, 92, 90, 255);
        case dlg::NodeType::End:       return IM_COL32(120, 52, 58, 255);
        default:                       return IM_COL32(72, 72, 84, 255);
    }
}
// A short body preview so the graph reads at a glance.
std::string DlgNodePreview(const dlg::Node& n) {
    switch (n.type) {
        case dlg::NodeType::Line: {
            std::string s = n.speaker.empty() ? n.text : (n.speaker + ": " + n.text);
            if (s.size() > 40) s = s.substr(0, 37) + "...";
            return s;
        }
        case dlg::NodeType::Condition: {
            const char* op[] = {"!=0", "==", "!=", ">", "<", ">=", "<="};
            const int oi = static_cast<int>(n.op);
            if (n.op == dlg::CmpOp::NotZero) return n.flag + " != 0";
            return n.flag + " " + (oi >= 0 && oi < 7 ? op[oi] : "?") + " " +
                   std::to_string(static_cast<int>(n.value));
        }
        case dlg::NodeType::SetFlag:
            return n.setFlag + " = " + std::to_string(static_cast<int>(n.setValue));
        default:
            return {};
    }
}
} // namespace

void Editor::OpenDialogue(const std::filesystem::path& path) {
    dlg::Graph g;
    if (!dlg::LoadGraph(path, g) || g.nodes.empty()) {
        // Fresh / unreadable: seed with Start -> Line so there's something to wire.
        g = dlg::Graph{};
        const u32 s = g.AddNode(dlg::NodeType::Start, {48.0f, 60.0f});
        const u32 l = g.AddNode(dlg::NodeType::Line, {280.0f, 60.0f});
        g.Connect(s, 0, l, 0);
    }
    dlgGraph_ = std::move(g);
    editedDialoguePath_ = path;
    dlgDirty_ = false;
    dlgFocus_ = true;
    dlgPan_ = glm::vec2(0.0f);
    dlgSelected_ = 0;
    dlgDragging_ = false;
    panelOpen_[Panel_DialogueEditor] = true;
}

bool Editor::SaveDialogue() {
    if (editedDialoguePath_.empty()) return false;
    if (!dlg::SaveGraph(editedDialoguePath_, dlgGraph_)) {
        HBE_ERROR("Failed to write dialogue graph '{}'.", editedDialoguePath_.string());
        return false;
    }
    StampNewAsset(editedDialoguePath_); // the save rebuilt the JSON; restore its id
    dlgDirty_ = false;
    assetsDirty_ = true;
    return true;
}

void Editor::DrawDialogueEditor(Engine& engine) {
    (void)engine;
    if (!panelOpen_[Panel_DialogueEditor]) return;
    if (dlgFocus_) {
        ImGui::SetNextWindowFocus();
        dlgFocus_ = false;
    }
    const bool dlgVisible = ImGui::Begin("Dialogue Editor", &panelOpen_[Panel_DialogueEditor]);
    // CLAIM Ctrl+S FOR THIS PANEL. Before the "no graph open" early return below on
    // purpose: an unconditional claim is what makes an empty Dialogue Editor say
    // "nothing open to save" instead of quietly writing the LEVEL. RouteFromRootWindow
    // means the ##dlgcanvas child does not split the claim off the panel.
    //
    // This REPLACES the old hand-rolled IsWindowFocused()+IsKeyPressed() handler that
    // used to sit at the bottom of this function: because ImGui::IsKeyPressed() is not
    // consumed by reading it, that handler ran IN ADDITION to the unguarded global
    // Ctrl+S, so one keypress here saved the graph AND the scene. It also had no
    // KeyShift test, so Ctrl+Shift+S saved the graph a second time on top of SaveAll.
    //
    // ...and above the `Begin() == false` early return too: a COLLAPSED window keeps
    // the focus (the collapse arrow focuses first, then collapses), so a claim below
    // that return would be skipped and the global route would write the LEVEL while
    // the focused window is titled "Dialogue Editor".
    ClaimSave(editor::SaveSurface::Dialogue);
    if (!dlgVisible) {
        ImGui::End();
        return;
    }

    // Toolbar: New / Open / Save + open file name.
    if (ImGui::Button("New")) {
        const std::filesystem::path p = CreateDialogueAsset();
        if (!p.empty()) OpenDialogue(p);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open")) ImGui::OpenPopup("##dlgopen");
    ImGui::SameLine();
    ImGui::BeginDisabled(editedDialoguePath_.empty());
    if (ImGui::Button(dlgDirty_ ? "Save*" : "Save")) {
        if (SaveDialogue())
            SetSaveStatus("Saved dialogue graph '" +
                              editedDialoguePath_.filename().string() + "'.",
                          false);
        else
            SetSaveStatus("DIALOGUE SAVE FAILED - '" +
                              editedDialoguePath_.filename().string() + "' was NOT written.",
                          true);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (editedDialoguePath_.empty()) {
        ImGui::TextDisabled("(no dialogue open)");
    } else {
        ImGui::Text("%s", editedDialoguePath_.filename().string().c_str());
        if (dlgDirty_) {
            ImGui::SameLine();
            ImGui::TextDisabled("(unsaved - Ctrl+S)");
        }
    }

    if (ImGui::BeginPopup("##dlgopen")) {
        bool any = false;
        for (const std::string& rel : ListAssetsByExt(".hbdialogue")) {
            any = true;
            if (ImGui::Selectable(rel.c_str())) {
                OpenDialogue(Project::Active().AssetsDir() / rel);
                ImGui::CloseCurrentPopup();
            }
        }
        if (!any) ImGui::TextDisabled("No .hbdialogue assets yet - use New.");
        ImGui::EndPopup();
    }

    if (editedDialoguePath_.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped(
            "Create or open a dialogue, then wire Start -> Line -> ... nodes. A Choice "
            "node presents clickable options (one branch each); Condition branches on a "
            "story flag; Set Flag writes one. Run it from a schematic \"Play Dialogue\" node.");
        ImGui::TextDisabled("Right-click the canvas to add a node. Drag from a pin to wire.");
        ImGui::End();
        return;
    }

    ImGui::Separator();
    const float kInspW = 330.0f;
    float canvasW = ImGui::GetContentRegionAvail().x - kInspW - 8.0f;
    if (canvasW < 120.0f) canvasW = ImGui::GetContentRegionAvail().x; // too narrow: canvas only
    DrawDialogueCanvas(canvasW);
    ImGui::SameLine();

    // --- Node inspector -----------------------------------------------------
    ImGui::BeginChild("##dlginsp", ImVec2(0, 0), ImGuiChildFlags_Borders);
    dlg::Node* n = dlgSelected_ ? dlgGraph_.Find(dlgSelected_) : nullptr;
    if (!n) {
        ImGui::TextDisabled("Select a node to edit it.");
    } else {
        ImGui::TextUnformatted(dlg::NodeTypeName(n->type));
        ImGui::Separator();
        char buf[1024];
        if (n->type == dlg::NodeType::Line) {
            std::snprintf(buf, sizeof(buf), "%s", n->speaker.c_str());
            if (ImGui::InputText("Speaker", buf, 96)) { n->speaker = buf; dlgDirty_ = true; }
            std::snprintf(buf, sizeof(buf), "%s", n->text.c_str());
            if (ImGui::InputTextMultiline("Text", buf, sizeof(buf), ImVec2(-1.0f, 80.0f))) {
                n->text = buf;
                dlgDirty_ = true;
            }
            {
                std::string cpick; // searchable voice-clip picker
                if (AssetPicker("Clip", n->clip, ".uaf", uaf::AssetType::Audio, cpick)) {
                    n->clip = cpick;
                    dlgDirty_ = true;
                }
            }
            if (ImGui::DragFloat("Hold (s, 0=auto)", &n->hold, 0.05f, 0.0f, 60.0f)) dlgDirty_ = true;
        } else if (n->type == dlg::NodeType::Choice) {
            ImGui::TextDisabled("Each option is a branch taken when the player picks it.");
            int removeIdx = -1;
            for (u32 i = 0; i < n->choices.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                dlg::ChoiceOption& c = n->choices[i];
                std::snprintf(buf, sizeof(buf), "%s", c.text.c_str());
                if (ImGui::InputText("Option", buf, 256)) { c.text = buf; dlgDirty_ = true; }
                std::snprintf(buf, sizeof(buf), "%s", c.showIf.c_str());
                if (ImGui::InputText("Show if flag", buf, 96)) { c.showIf = buf; dlgDirty_ = true; }
                if (ImGui::SmallButton("Remove option")) removeIdx = static_cast<int>(i);
                ImGui::Separator();
                ImGui::PopID();
            }
            if (removeIdx >= 0 && n->choices.size() > 1) {
                // Drop the option AND keep wire pin indices aligned: remove links on
                // that pin, shift higher pins down by one.
                const u32 k = static_cast<u32>(removeIdx);
                for (usize li = 0; li < dlgGraph_.links.size();) {
                    dlg::Link& l = dlgGraph_.links[li];
                    if (l.fromNode == n->id && l.fromPin == k)
                        dlgGraph_.links.erase(dlgGraph_.links.begin() + static_cast<std::ptrdiff_t>(li));
                    else {
                        if (l.fromNode == n->id && l.fromPin > k) --l.fromPin;
                        ++li;
                    }
                }
                n->choices.erase(n->choices.begin() + removeIdx);
                dlgDirty_ = true;
            }
            if (ImGui::Button("+ Add option")) {
                n->choices.push_back({"New option", ""});
                dlgDirty_ = true;
            }
        } else if (n->type == dlg::NodeType::Condition) {
            std::snprintf(buf, sizeof(buf), "%s", n->flag.c_str());
            if (ImGui::InputText("Flag", buf, 96)) { n->flag = buf; dlgDirty_ = true; }
            const char* kOps = "!= 0\0==\0!=\0>\0<\0>=\0<=\0";
            int op = static_cast<int>(n->op);
            if (ImGui::Combo("Test", &op, kOps)) {
                n->op = static_cast<dlg::CmpOp>(op);
                dlgDirty_ = true;
            }
            if (n->op != dlg::CmpOp::NotZero) {
                if (ImGui::DragFloat("Value", &n->value, 0.1f)) dlgDirty_ = true;
            }
            ImGui::TextDisabled("Pin 0 = True, Pin 1 = False.");
        } else if (n->type == dlg::NodeType::SetFlag) {
            std::snprintf(buf, sizeof(buf), "%s", n->setFlag.c_str());
            if (ImGui::InputText("Flag", buf, 96)) { n->setFlag = buf; dlgDirty_ = true; }
            if (ImGui::DragFloat("Set to", &n->setValue, 0.1f)) dlgDirty_ = true;
        } else {
            ImGui::TextDisabled("%s node has no fields.", dlg::NodeTypeName(n->type));
        }
    }
    ImGui::EndChild();
    // (Ctrl+S is claimed at the top of this function - see ClaimSave there.)
    ImGui::End();
}

void Editor::DrawDialogueCanvas(float width) {
    using dlg::NodeType;
    const float NW = 210.0f, TITLEH = 24.0f, ROWH = 22.0f, PAD = 8.0f, PINR = 5.0f;
    const ImU32 kExec = IM_COL32(235, 235, 235, 255);

    if (width < 80.0f) width = 80.0f;
    ImGui::BeginChild("##dlgcanvas", ImVec2(width, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoMove);
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cP0 = ImGui::GetCursorScreenPos();
    const ImVec2 cSz = ImGui::GetContentRegionAvail();
    const ImVec2 cP1(cP0.x + cSz.x, cP0.y + cSz.y);
    const ImVec2 mp = io.MousePos;
    const bool canvasHovered = ImGui::IsWindowHovered();

    // Background + grid.
    dl->AddRectFilled(cP0, cP1, IM_COL32(26, 27, 32, 255));
    const float grid = 24.0f;
    const ImU32 gridCol = IM_COL32(40, 42, 48, 255);
    for (float x = std::fmod(dlgPan_.x, grid); x < cSz.x; x += grid)
        dl->AddLine(ImVec2(cP0.x + x, cP0.y), ImVec2(cP0.x + x, cP1.y), gridCol);
    for (float y = std::fmod(dlgPan_.y, grid); y < cSz.y; y += grid)
        dl->AddLine(ImVec2(cP0.x, cP0.y + y), ImVec2(cP1.x, cP0.y + y), gridCol);
    dl->AddRect(cP0, cP1, IM_COL32(12, 12, 16, 255));

    const ImVec2 origin(cP0.x + dlgPan_.x, cP0.y + dlgPan_.y);
    auto toScreen = [&](glm::vec2 p) { return ImVec2(origin.x + p.x, origin.y + p.y); };
    auto hasInput = [](const dlg::Node& n) { return n.type != NodeType::Start; };
    auto rowCenterY = [&](const dlg::Node& n, int i) {
        return toScreen(n.pos).y + TITLEH + ROWH * i + ROWH * 0.5f;
    };
    auto inPinPos = [&](const dlg::Node& n) { return ImVec2(toScreen(n.pos).x, rowCenterY(n, 0)); };
    auto outPinPos = [&](const dlg::Node& n, int i) {
        return ImVec2(toScreen(n.pos).x + NW, rowCenterY(n, i));
    };
    auto dist2 = [](ImVec2 a, ImVec2 b) {
        const float dx = a.x - b.x, dy = a.y - b.y;
        return dx * dx + dy * dy;
    };

    // Hovered pin (nearest within a small radius).
    struct PinHover { u32 node = 0, pin = 0; bool isOutput = false; bool valid = false; };
    PinHover hov;
    {
        float best = (PINR + 4.0f) * (PINR + 4.0f);
        for (const dlg::Node& n : dlgGraph_.nodes) {
            if (hasInput(n)) {
                const float dd = dist2(mp, inPinPos(n));
                if (dd < best) { best = dd; hov = {n.id, 0, false, true}; }
            }
            const int outs = dlgGraph_.OutPinCount(n);
            for (int i = 0; i < outs; ++i) {
                const float dd = dist2(mp, outPinPos(n, i));
                if (dd < best) { best = dd; hov = {n.id, static_cast<u32>(i), true, true}; }
            }
        }
    }

    // Links (bezier) + link hover.
    auto bez = [](ImVec2 p0, ImVec2 c0, ImVec2 c1, ImVec2 p1, float t) {
        const float u = 1.0f - t;
        const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        return ImVec2(w0 * p0.x + w1 * c0.x + w2 * c1.x + w3 * p1.x,
                      w0 * p0.y + w1 * c0.y + w2 * c1.y + w3 * p1.y);
    };
    int hoveredLink = -1;
    for (u32 li = 0; li < dlgGraph_.links.size(); ++li) {
        const dlg::Link& l = dlgGraph_.links[li];
        const dlg::Node* a = dlgGraph_.Find(l.fromNode);
        const dlg::Node* b = dlgGraph_.Find(l.toNode);
        if (!a || !b) continue;
        if (static_cast<int>(l.fromPin) >= dlgGraph_.OutPinCount(*a) || !hasInput(*b)) continue;
        const ImVec2 p0 = outPinPos(*a, static_cast<int>(l.fromPin));
        const ImVec2 p1 = inPinPos(*b);
        const float dx = std::max(40.0f, std::fabs(p1.x - p0.x) * 0.5f);
        const ImVec2 c0(p0.x + dx, p0.y), c1(p1.x - dx, p1.y);
        float md = 1e9f;
        for (int s = 0; s <= 16; ++s) md = std::min(md, dist2(mp, bez(p0, c0, c1, p1, s / 16.0f)));
        const bool lh = canvasHovered && md < 36.0f && !hov.valid;
        if (lh) hoveredLink = static_cast<int>(li);
        dl->AddBezierCubic(p0, c0, c1, p1, lh ? IM_COL32(255, 200, 90, 255) : kExec, lh ? 3.5f : 2.2f);
    }

    // Nodes.
    for (dlg::Node& n : dlgGraph_.nodes) {
        const int outs = dlgGraph_.OutPinCount(n);
        const int rows = std::max(1, std::max(hasInput(n) ? 1 : 0, outs));
        const ImVec2 nMin = toScreen(n.pos);
        const ImVec2 nMax(nMin.x + NW, nMin.y + TITLEH + ROWH * rows + PAD);
        const bool selected = (dlgSelected_ == n.id);

        dl->AddRectFilled(nMin, nMax, IM_COL32(42, 44, 52, 240), 5.0f);
        dl->AddRectFilled(nMin, ImVec2(nMax.x, nMin.y + TITLEH), DlgNodeColor(n.type), 5.0f,
                          ImDrawFlags_RoundCornersTop);
        dl->AddRect(nMin, nMax, selected ? IM_COL32(255, 200, 80, 255) : IM_COL32(18, 18, 22, 255),
                    5.0f, 0, selected ? 2.5f : 1.2f);
        dl->AddText(ImVec2(nMin.x + 9, nMin.y + 4), IM_COL32(240, 240, 245, 255),
                    dlg::NodeTypeName(n.type));

        // Body preview (Line/Condition/SetFlag).
        const std::string preview = DlgNodePreview(n);
        if (!preview.empty())
            dl->AddText(ImVec2(nMin.x + 10, nMin.y + TITLEH + 3), IM_COL32(185, 190, 205, 255),
                        preview.c_str());

        ImGui::PushID(static_cast<int>(n.id));
        ImGui::SetCursorScreenPos(nMin);
        ImGui::InvisibleButton("title", ImVec2(NW, TITLEH));
        if (ImGui::IsItemActivated()) dlgSelected_ = n.id;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            n.pos.x += io.MouseDelta.x;
            n.pos.y += io.MouseDelta.y;
            dlgDirty_ = true;
        }

        // Input pin (left, at row 0).
        if (hasInput(n)) {
            const ImVec2 pp = inPinPos(n);
            dl->AddTriangleFilled(ImVec2(pp.x - 4, pp.y - 5), ImVec2(pp.x - 4, pp.y + 5),
                                  ImVec2(pp.x + 5, pp.y), kExec);
        }
        // Output pins (right) + labels (Choice option text / Condition True/False).
        for (int i = 0; i < outs; ++i) {
            const ImVec2 pp = outPinPos(n, i);
            dl->AddTriangleFilled(ImVec2(pp.x - 5, pp.y - 5), ImVec2(pp.x - 5, pp.y + 5),
                                  ImVec2(pp.x + 4, pp.y), kExec);
            std::string lbl;
            if (n.type == NodeType::Choice && i < static_cast<int>(n.choices.size())) {
                lbl = n.choices[static_cast<usize>(i)].text;
                if (lbl.size() > 16) lbl = lbl.substr(0, 15) + "\xE2\x80\xA6"; // ellipsis
            } else if (n.type == NodeType::Condition) {
                lbl = (i == 0) ? "True" : "False";
            }
            if (!lbl.empty()) {
                const float tw = ImGui::CalcTextSize(lbl.c_str()).x;
                dl->AddText(ImVec2(pp.x - 10 - tw, pp.y - 7), IM_COL32(200, 202, 210, 255),
                            lbl.c_str());
            }
        }
        ImGui::PopID();
    }

    // In-progress wire.
    if (dlgDragging_) {
        if (const dlg::Node* s = dlgGraph_.Find(dlgDragNode_)) {
            const ImVec2 sp = dlgDragFromOutput_ ? outPinPos(*s, static_cast<int>(dlgDragPin_))
                                                 : inPinPos(*s);
            const float dx = std::max(40.0f, std::fabs(mp.x - sp.x) * 0.5f);
            const float dir = dlgDragFromOutput_ ? 1.0f : -1.0f;
            dl->AddBezierCubic(sp, ImVec2(sp.x + dx * dir, sp.y), ImVec2(mp.x - dx * dir, mp.y), mp,
                               IM_COL32(255, 235, 150, 255), 2.4f);
        } else {
            dlgDragging_ = false;
        }
    }

    // Interaction: start / re-pick a wire.
    if (canvasHovered && hov.valid && !dlgDragging_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool started = false;
        if (!hov.isOutput) {
            for (u32 i = 0; i < dlgGraph_.links.size(); ++i) {
                const dlg::Link& l = dlgGraph_.links[i];
                if (l.toNode == hov.node && l.toPin == hov.pin) {
                    dlgDragNode_ = l.fromNode;
                    dlgDragPin_ = l.fromPin;
                    dlgDragFromOutput_ = true;
                    dlgGraph_.RemoveLink(i);
                    dlgDirty_ = true;
                    started = true;
                    break;
                }
            }
        }
        if (!started) {
            dlgDragNode_ = hov.node;
            dlgDragPin_ = hov.pin;
            dlgDragFromOutput_ = hov.isOutput;
        }
        dlgDragging_ = true;
    }
    // Release: connect to an opposite-orientation pin.
    if (dlgDragging_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (hov.valid && hov.isOutput != dlgDragFromOutput_) {
            u32 fn, fp, tn, tp;
            if (dlgDragFromOutput_) { fn = dlgDragNode_; fp = dlgDragPin_; tn = hov.node; tp = hov.pin; }
            else { fn = hov.node; fp = hov.pin; tn = dlgDragNode_; tp = dlgDragPin_; }
            if (dlgGraph_.Connect(fn, fp, tn, tp)) dlgDirty_ = true;
        }
        dlgDragging_ = false;
    }

    // Empty-canvas click deselects.
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hov.valid &&
        !ImGui::IsAnyItemHovered())
        dlgSelected_ = 0;

    // Pan: middle-drag or left-drag empty space.
    if (canvasHovered && !dlgDragging_ && !ImGui::IsAnyItemActive()) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
            (!hov.valid && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f))) {
            dlgPan_.x += io.MouseDelta.x;
            dlgPan_.y += io.MouseDelta.y;
        }
    }

    // Delete the selected node.
    if (dlgSelected_ && ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        dlgGraph_.RemoveNode(dlgSelected_);
        dlgSelected_ = 0;
        dlgDirty_ = true;
    }

    // Right-click: delete a hovered link, else open the add-node menu.
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hoveredLink >= 0) {
            dlgGraph_.RemoveLink(static_cast<u32>(hoveredLink));
            dlgDirty_ = true;
        } else if (!hov.valid) {
            dlgAddPos_ = glm::vec2(mp.x - origin.x, mp.y - origin.y);
            ImGui::OpenPopup("##dlgadd");
        }
    }
    if (ImGui::BeginPopup("##dlgadd")) {
        for (int i = 0; i < static_cast<int>(NodeType::Count); ++i) {
            const NodeType t = static_cast<NodeType>(i);
            if (ImGui::MenuItem(dlg::NodeTypeName(t))) {
                dlgSelected_ = dlgGraph_.AddNode(t, dlgAddPos_);
                dlgDirty_ = true;
            }
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

} // namespace hbe
