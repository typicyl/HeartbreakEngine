// Collab/Journal.cpp
#include "Collab/Journal.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace hbe::collab {

namespace fs = std::filesystem;

namespace {

constexpr u32 kMagic = 0x314A4248; // "HBJ1"
constexpr u16 kRecordVersion = 1;
// Bounds a single record BEFORE anything is allocated from its length. A corrupt or
// hostile length is otherwise an instant multi-gigabyte reserve.
constexpr u32 kMaxRecordBytes = 64u * 1024u * 1024u;

// Every record: {u32 length, u16 kind, u16 version} then the payload, then an opaque
// `ext` blob.
//
// THE EXT BLOB IS NOT OPTIONAL AND NOT DECORATION. BinaryReader::Ok() latches only on
// UNDERFLOW - it never checks for trailing bytes - so a record that grows a field
// decodes as SUCCESSFUL on an older reader, which then acts on a half-understood
// struct. Silently. Every record therefore ends with a length-prefixed blob an old
// reader skips and a new one may fill, which is the only way to extend a record
// without minting a new kind.
struct RecHeader {
    u32 length = 0;
    u16 kind = 0;
    u16 version = kRecordVersion;
};
constexpr usize kRecHeaderBytes = 8;

void PutId(BinaryWriter& w, const CommitId& id) {
    w.Pod(id.peer);
    w.Pod(id.n);
}
bool GetId(BinaryReader& r, CommitId& id) { return r.Pod(id.peer) && r.Pod(id.n); }

void EmitRecord(std::vector<u8>& out, RecKind kind, const BinaryWriter& body) {
    const std::vector<u8>& b = body.Data();
    if (b.size() > kMaxRecordBytes) return; // refuse to write what we must reject
    RecHeader h;
    h.length = static_cast<u32>(b.size());
    h.kind = static_cast<u16>(kind);
    const usize base = out.size();
    out.resize(base + kRecHeaderBytes + b.size());
    std::memcpy(out.data() + base, &h.length, 4);
    std::memcpy(out.data() + base + 4, &h.kind, 2);
    std::memcpy(out.data() + base + 6, &h.version, 2);
    if (!b.empty()) std::memcpy(out.data() + base + kRecHeaderBytes, b.data(), b.size());
}

} // namespace

std::string CommitId::ToString() const {
    char b[48];
    std::snprintf(b, sizeof(b), "%016llx:%llu", static_cast<unsigned long long>(peer),
                  static_cast<unsigned long long>(n));
    return b;
}

const char* ChangeOpName(ChangeOp o) {
    switch (o) {
        case ChangeOp::Set: return "set";
        case ChangeOp::Remove: return "remove";
        case ChangeOp::CreateEntity: return "create";
        case ChangeOp::DeleteEntity: return "delete";
    }
    return "?";
}

const char* MergeVerdictName(MergeVerdict v) {
    switch (v) {
        case MergeVerdict::UpToDate: return "UpToDate";
        case MergeVerdict::FastForward: return "FastForward";
        case MergeVerdict::Merge: return "Merge";
        case MergeVerdict::NeedsReview: return "NeedsReview";
        case MergeVerdict::RefusedEpoch: return "RefusedEpoch";
        case MergeVerdict::RefusedDocument: return "RefusedDocument";
    }
    return "?";
}

bool Journal::Append(const fs::path& file, const Commit& c) {
    std::vector<u8> bytes;
    {
        BinaryWriter w;
        PutId(w, c.id);
        PutId(w, c.parent);
        w.Pod(c.doc);
        w.Pod(c.guidEpoch);
        w.Pod(c.timestampMs);
        w.Str(c.author);
        w.Str(c.message);
        w.Pod(static_cast<u32>(c.changes.size()));
        w.Str(std::string()); // ext - see RecHeader
        EmitRecord(bytes, RecKind::CommitBegin, w);
    }
    for (const Change& ch : c.changes) {
        BinaryWriter w;
        w.Pod(ch.guid);
        w.Str(ch.component);
        w.Pod(static_cast<u8>(ch.op));
        w.Str(ch.before);
        w.Str(ch.after);
        w.Str(std::string()); // ext
        EmitRecord(bytes, RecKind::Change, w);
    }
    {
        BinaryWriter w;
        PutId(w, c.id);
        w.Str(std::string()); // ext
        EmitRecord(bytes, RecKind::CommitEnd, w);
    }

    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const bool fresh = !fs::exists(file, ec);
    // APPEND, never rewrite. A crash mid-append truncates the TAIL, which Load detects
    // and discards; rewriting the whole file in place would instead risk a hole in the
    // middle, which is undetectable and costs every commit rather than the last one.
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) return false;
    if (fresh) {
        out.write(reinterpret_cast<const char*>(&kMagic), 4);
        if (!out.good()) return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out.good()) return false;
    out.flush(); // the work this file exists to protect must not sit in a buffer
    commits_.push_back(c);
    return true;
}

bool Journal::Load(const fs::path& file, bool* outTruncated) {
    commits_.clear();
    if (outTruncated) *outTruncated = false;
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff size = in.tellg();
    if (size < 4) return false;
    in.seekg(0);
    std::vector<u8> data(static_cast<usize>(size));
    in.read(reinterpret_cast<char*>(data.data()), size);
    if (!in) return false;

    u32 magic = 0;
    std::memcpy(&magic, data.data(), 4);
    if (magic != kMagic) return false;

    usize p = 4;
    Commit cur;
    bool inCommit = false;
    u32 wantChanges = 0;

    while (p < data.size()) {
        if (data.size() - p < kRecHeaderBytes) { // torn header
            if (outTruncated) *outTruncated = true;
            break;
        }
        RecHeader h;
        std::memcpy(&h.length, data.data() + p, 4);
        std::memcpy(&h.kind, data.data() + p + 4, 2);
        std::memcpy(&h.version, data.data() + p + 6, 2);
        if (h.length > kMaxRecordBytes || data.size() - p - kRecHeaderBytes < h.length) {
            // Torn or absurd: everything from here on is unreadable. Keep what came
            // before - that is the whole point of append-only.
            if (outTruncated) *outTruncated = true;
            break;
        }
        const u8* payload = data.data() + p + kRecHeaderBytes;
        BinaryReader r(payload, h.length);
        const RecKind kind = static_cast<RecKind>(h.kind);

        if (kind == RecKind::CommitBegin) {
            Commit c;
            u32 n = 0;
            std::string ext;
            GetId(r, c.id);
            GetId(r, c.parent);
            r.Pod(c.doc);
            r.Pod(c.guidEpoch);
            r.Pod(c.timestampMs);
            r.Str(c.author);
            r.Str(c.message);
            r.Pod(n);
            r.Str(ext);
            if (!r.Ok()) { if (outTruncated) *outTruncated = true; break; }
            cur = std::move(c);
            wantChanges = n;
            inCommit = true;
        } else if (kind == RecKind::Change && inCommit) {
            Change ch;
            u8 op = 0;
            std::string ext;
            r.Pod(ch.guid);
            r.Str(ch.component);
            r.Pod(op);
            r.Str(ch.before);
            r.Str(ch.after);
            r.Str(ext);
            if (!r.Ok() || op > static_cast<u8>(ChangeOp::DeleteEntity)) {
                if (outTruncated) *outTruncated = true;
                break;
            }
            ch.op = static_cast<ChangeOp>(op);
            cur.changes.push_back(std::move(ch));
        } else if (kind == RecKind::CommitEnd && inCommit) {
            CommitId id;
            GetId(r, id);
            if (!r.Ok()) { if (outTruncated) *outTruncated = true; break; }
            // A commit counts only when its END record is present AND the change count
            // matches. A crash between CommitBegin and CommitEnd therefore drops a
            // PARTIAL commit rather than committing half an artist's edit - which would
            // be worse than losing all of it, because nobody would know.
            if (id == cur.id && cur.changes.size() == wantChanges) commits_.push_back(cur);
            else if (outTruncated) *outTruncated = true;
            inCommit = false;
            cur = Commit{};
        }
        // An UNKNOWN kind is skipped by its length - the reason the header carries one.
        p += kRecHeaderBytes + h.length;
    }
    // An unterminated commit at the end is a torn tail, by definition.
    if (inCommit && outTruncated) *outTruncated = true;
    return true;
}

CommitId Journal::Head() const {
    return commits_.empty() ? CommitId{} : commits_.back().id;
}

const Commit* Journal::Find(const CommitId& id) const {
    for (const Commit& c : commits_)
        if (c.id == id) return &c;
    return nullptr;
}

MergePlan PlanMerge(const Journal& mine, const Journal& theirs, DocId doc, u64 guidEpoch) {
    MergePlan plan;

    // 1) SAME DOCUMENT? Two different scenes have nothing to say to each other.
    for (const Commit& c : theirs.Commits()) {
        if (c.doc != doc) {
            plan.verdict = MergeVerdict::RefusedDocument;
            plan.explanation = "These commits belong to a different scene.";
            return plan;
        }
    }

    // 2) THE EPOCH GUARD, before anything else that could act on a guid.
    //
    // A guid is only meaningful relative to the migration that minted it. Two copies of
    // a pre-guid scene migrated independently produce guids that COLLIDE BY
    // CONSTRUCTION (Derive is a pure function of path and row index), so comparing them
    // would confidently match unrelated entities. Refuse, and say exactly why.
    for (const Commit& c : theirs.Commits()) {
        if (c.guidEpoch != 0 && guidEpoch != 0 && c.guidEpoch != guidEpoch) {
            plan.verdict = MergeVerdict::RefusedEpoch;
            plan.explanation =
                "These edits came from a copy of the scene whose entity ids were "
                "generated separately, so the same id means different objects on each "
                "side. Merging would silently move the wrong things. Re-share the scene "
                "from one copy and start again.";
            return plan;
        }
    }

    // 3) What do they have that we lack?
    std::unordered_set<std::string> haveMine;
    for (const Commit& c : mine.Commits()) haveMine.insert(c.id.ToString());
    for (const Commit& c : theirs.Commits())
        if (!haveMine.count(c.id.ToString())) plan.toApply.push_back(c);

    if (plan.toApply.empty()) {
        plan.verdict = MergeVerdict::UpToDate;
        plan.explanation = "Nothing new.";
        return plan;
    }

    // 4) Did WE diverge? Anything of ours they lack means both sides moved.
    std::unordered_set<std::string> haveTheirs;
    for (const Commit& c : theirs.Commits()) haveTheirs.insert(c.id.ToString());
    std::vector<const Commit*> onlyMine;
    for (const Commit& c : mine.Commits())
        if (!haveTheirs.count(c.id.ToString())) onlyMine.push_back(&c);

    if (onlyMine.empty()) {
        plan.verdict = MergeVerdict::FastForward;
        plan.explanation = "Applying " + std::to_string(plan.toApply.size()) +
                           " change set(s) from the others.";
        return plan;
    }

    // 5) Both moved. Overlapping (entity, component) is the ONLY thing a machine may
    //    not decide - disjoint keys merge cleanly no matter how many there are.
    struct Touch {
        const Change* ch;
        const Commit* c;
    };
    std::unordered_map<std::string, Touch> mineTouched;
    const auto key = [](u64 g, const std::string& comp) {
        return std::to_string(g) + "/" + comp;
    };
    for (const Commit* c : onlyMine)
        for (const Change& ch : c->changes) mineTouched[key(ch.guid, ch.component)] = {&ch, c};

    for (const Commit& c : plan.toApply) {
        for (const Change& ch : c.changes) {
            const auto it = mineTouched.find(key(ch.guid, ch.component));
            if (it == mineTouched.end()) continue;
            // SAME KEY, BOTH SIDES. If they also agree on the resulting value there is
            // nothing to ask about - two people nudging one object to the same place is
            // not a conflict, and asking would train people to click through.
            if (it->second.ch->after == ch.after) continue;
            Conflict cf;
            cf.guid = ch.guid;
            cf.component = ch.component;
            // `before` is why this is reviewable rather than merely detectable.
            cf.base = ch.before;
            cf.mine = it->second.ch->after;
            cf.theirs = ch.after;
            cf.mineAuthor = it->second.c->author;
            cf.theirsAuthor = c.author;
            plan.conflicts.push_back(std::move(cf));
        }
    }

    if (!plan.conflicts.empty()) {
        plan.verdict = MergeVerdict::NeedsReview;
        plan.explanation = std::to_string(plan.conflicts.size()) +
                           " change(s) were edited by both sides and need a decision.";
        return plan;
    }
    plan.verdict = MergeVerdict::Merge;
    plan.explanation = "Both sides changed different things; combining them.";
    return plan;
}

} // namespace hbe::collab
