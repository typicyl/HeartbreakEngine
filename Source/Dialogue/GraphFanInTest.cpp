// Dialogue/GraphFanInTest.cpp - the RECONVERGENCE contract (--test-graphfanin).
//
// Both node graphs in this engine (dialogue and schematic) used to enforce "one wire
// per INPUT pin" as well as "one wire per output pin". For data pins that rule is
// correct - a value input reads exactly one source. For EXECUTION pins it is wrong,
// and destructively so: wiring a second branch into a node that already had an
// incoming exec wire SILENTLY DELETED the first, with nothing on screen to say so.
//
// That made the single most common graph shape unauthorable - a choice fanning out to
// several branches which then REJOIN a shared tail - and at runtime the orphaned
// branch hit Follow()==0 and ended the conversation early.
//
// This pins the corrected rule for both graphs, and every case also asserts that the
// OLD behaviour would fail it, so the test measures the fix rather than the status quo.
//
// Headless: no GPU, no window, no project.
#include "Dialogue/DialogueGraph.h"

#include "Core/Log.h"
#include "Schematic/Schematic.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace hbe::dlg {
namespace {

bool g_ok = true;

void Check(bool cond, const char* what) {
    if (!cond) {
        g_ok = false;
        HBE_ERROR("graphfanin: FAILED - {}", what);
    }
}

// How many links land on this node's input pin.
u32 InCount(const Graph& g, u32 node, u32 pin) {
    u32 n = 0;
    for (const Link& l : g.links)
        if (l.toNode == node && l.toPin == pin) ++n;
    return n;
}

u32 SkInCount(const schematic::Graph& g, u32 node, u32 pin) {
    u32 n = 0;
    for (const schematic::Link& l : g.links)
        if (l.toNode == node && l.toPin == pin) ++n;
    return n;
}

} // namespace

bool GraphFanInSelfTest() {
    g_ok = true;

    // ---------------------------------------------------------------------------
    // 1) DIALOGUE: choice -> two branches -> rejoin. The canonical shape.
    // ---------------------------------------------------------------------------
    {
        Graph g;
        const u32 start = g.AddNode(NodeType::Start, {0.0f, 0.0f});
        const u32 choice = g.AddNode(NodeType::Choice, {1.0f, 0.0f});
        if (Node* c = g.Find(choice)) c->choices = {ChoiceOption{"A", ""}, ChoiceOption{"B", ""}};
        const u32 branchA = g.AddNode(NodeType::Line, {2.0f, -1.0f});
        const u32 branchB = g.AddNode(NodeType::Line, {2.0f, 1.0f});
        const u32 rejoin = g.AddNode(NodeType::Line, {3.0f, 0.0f});
        const u32 end = g.AddNode(NodeType::End, {4.0f, 0.0f});

        Check(g.Connect(start, 0, choice, 0), "start -> choice connects");
        Check(g.Connect(choice, 0, branchA, 0), "choice pin 0 -> branch A connects");
        Check(g.Connect(choice, 1, branchB, 0), "choice pin 1 -> branch B connects");

        // THE CASE THE OLD CODE BROKE: two branches into one input pin.
        Check(g.Connect(branchA, 0, rejoin, 0), "branch A -> rejoin connects");
        Check(g.Connect(branchB, 0, rejoin, 0), "branch B -> rejoin connects");
        Check(InCount(g, rejoin, 0) == 2,
              "BOTH branches stay wired to the rejoin node (old code left 1)");

        // ...and both are still traversable. This is the runtime consequence: under
        // the old rule one of these was 0 and that branch ended the conversation.
        Check(g.Follow(branchA, 0) == rejoin, "branch A still reaches the rejoin node");
        Check(g.Follow(branchB, 0) == rejoin, "branch B still reaches the rejoin node");

        Check(g.Connect(rejoin, 0, end, 0), "rejoin -> end connects");
        Check(g.Follow(rejoin, 0) == end, "the shared tail continues to End");
    }

    // ---------------------------------------------------------------------------
    // 2) DIALOGUE: an OUTPUT pin is still exclusive. Follow() returns THE next node,
    //    so a second wire off one output must replace the first, not accumulate.
    // ---------------------------------------------------------------------------
    {
        Graph g;
        const u32 line = g.AddNode(NodeType::Line, {0.0f, 0.0f});
        const u32 first = g.AddNode(NodeType::Line, {1.0f, 0.0f});
        const u32 second = g.AddNode(NodeType::Line, {1.0f, 1.0f});
        Check(g.Connect(line, 0, first, 0), "output connects once");
        Check(g.Connect(line, 0, second, 0), "re-connecting the same output succeeds");
        u32 outCount = 0;
        for (const Link& l : g.links)
            if (l.fromNode == line && l.fromPin == 0) ++outCount;
        Check(outCount == 1, "an output pin still drives exactly ONE wire");
        Check(g.Follow(line, 0) == second, "the newest wire wins on an output pin");
    }

    // ---------------------------------------------------------------------------
    // 3) DIALOGUE: removing a node drops every one of its in-edges, not just one.
    //    Fan-in makes this reachable: the rejoin node now has two.
    // ---------------------------------------------------------------------------
    {
        Graph g;
        const u32 a = g.AddNode(NodeType::Line, {0.0f, 0.0f});
        const u32 b = g.AddNode(NodeType::Line, {0.0f, 1.0f});
        const u32 shared = g.AddNode(NodeType::Line, {1.0f, 0.0f});
        g.Connect(a, 0, shared, 0);
        g.Connect(b, 0, shared, 0);
        Check(InCount(g, shared, 0) == 2, "two in-edges before removal");
        g.RemoveNode(shared);
        Check(g.links.empty(), "removing a fan-in node drops ALL of its links");
        Check(g.Follow(a, 0) == 0 && g.Follow(b, 0) == 0,
              "both former predecessors now lead nowhere");
    }

    // ---------------------------------------------------------------------------
    // 4) DIALOGUE: fan-in survives a save/load round trip.
    // ---------------------------------------------------------------------------
    {
        Graph g;
        const u32 a = g.AddNode(NodeType::Line, {0.0f, 0.0f});
        const u32 b = g.AddNode(NodeType::Line, {0.0f, 1.0f});
        const u32 shared = g.AddNode(NodeType::End, {1.0f, 0.0f});
        g.Connect(a, 0, shared, 0);
        g.Connect(b, 0, shared, 0);

        const auto tmp = std::filesystem::temp_directory_path() / "hbe_fanin_test.hbdialogue";
        Check(SaveGraph(tmp, g), "a fan-in graph saves");
        Graph loaded;
        Check(LoadGraph(tmp, loaded), "a fan-in graph loads");
        Check(InCount(loaded, shared, 0) == 2,
              "BOTH in-edges survive save/load (a lossy writer would drop one)");
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    }

    // ---------------------------------------------------------------------------
    // 5) SCHEMATIC: exec fan-in allowed, exec OUTPUT still exclusive.
    // ---------------------------------------------------------------------------
    {
        schematic::Graph g;
        // Two Branch nodes converging on one action is the shape that matters. Use
        // node types whose first input pin is Exec; Describe() drives the pin kinds.
        const u32 a = g.AddNode(schematic::NodeType::EventUpdate, {0.0f, 0.0f});
        const u32 b = g.AddNode(schematic::NodeType::EventStart, {0.0f, 1.0f});
        const u32 shared = g.AddNode(schematic::NodeType::Print, {1.0f, 0.0f});

        const bool c1 = g.Connect(a, 0, shared, 0);
        const bool c2 = g.Connect(b, 0, shared, 0);
        Check(c1 && c2, "two exec sources connect to one exec input");
        Check(SkInCount(g, shared, 0) == 2,
              "SCHEMATIC: both exec wires survive (old code kept only the last)");

        // The exec OUTPUT stays exclusive - it names THE next node.
        const u32 other = g.AddNode(schematic::NodeType::Print, {1.0f, 1.0f});
        g.Connect(a, 0, other, 0);
        u32 outFromA = 0;
        for (const schematic::Link& l : g.links)
            if (l.fromNode == a && l.fromPin == 0) ++outFromA;
        Check(outFromA == 1, "SCHEMATIC: an exec output still drives exactly one wire");
    }

    return g_ok;
}

} // namespace hbe::dlg
