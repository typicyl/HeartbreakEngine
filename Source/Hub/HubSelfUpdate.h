// Hub/HubSelfUpdate.h - the Hub replacing its own executable.
//
// THE ENGINE UPDATER CANNOT DO THIS. Its whole design rests on the Hub living OUTSIDE the
// directory it swaps: it renames <install>/bin out of the way, which is legal precisely
// because that is not the directory the running process lives in. The Hub has no such luxury
// about itself.
//
// What Windows actually forbids is DELETING or OVERWRITING a running executable. It expressly
// permits RENAMING one - the file object stays open and mapped, and the path simply changes
// underneath it. So a process can move itself aside and put a replacement where it used to
// be, with no helper process, no scheduled task and no batch file:
//
//   1. download   HeartbreakHub.new.exe   beside the running exe   (nothing live touched)
//   2. verify its SHA-256 against the manifest                     (nothing live touched)
//   3. rename     HeartbreakHub.exe    -> HeartbreakHub.old.exe    (reversible)
//   4. rename     HeartbreakHub.new.exe -> HeartbreakHub.exe
//   5. relaunch; the new process deletes the .old on the way up
//
// Steps 3 and 4 are same-directory renames: near-atomic, and step 3 is undone if step 4
// fails, so a failed self-update leaves a working Hub rather than none. Everything that can
// realistically fail - network, disk, hash mismatch - happens in 1-2 before anything live has
// moved. It is the same shape as the engine updater, applied to one file instead of a tree.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>

namespace hbe::hub {

// Where the running Hub actually is. Everything here is relative to this, never to the
// working directory - a launcher is routinely started from somewhere else entirely.
std::filesystem::path HubExePath();

// Deletes the leftover HeartbreakHub.old.exe from a previous self-update. Call once at boot,
// AFTER the new process is up: that is the first moment the old file is no longer running and
// can actually be removed. Failure is ignored on purpose - a stale .old is harmless clutter,
// and refusing to start over it would be absurd.
void CleanupAfterSelfUpdate();

// True when a .new is already staged and verified, waiting for the swap.
bool SelfUpdateStaged();

// Stage a Hub that arrived INSIDE the engine payload. Every engine archive ships the
// matching Hub, so the launcher and the engine it manages can never drift apart - and there
// is no second download, no second manifest entry to keep in sync, and no second version
// number to reason about.
//
// THE INTEGRITY RULE STILL HOLDS, it is just satisfied earlier. StageSelfUpdate refuses an
// unhashed executable because TLS alone does not say the server sent what was expected.
// Here the bytes were covered by the manifest's SHA-256 over the whole archive, verified
// before a single file was extracted - so this executable is checked by construction, and
// re-hashing the extracted copy against itself would prove nothing. That reasoning is the
// only thing that licenses skipping the hash argument; pointing this at an arbitrary path
// on disk would be defeating the check rather than satisfying it.
//
// Returns false with an EMPTY outError when the payload's Hub is byte-identical to the one
// already running - the common case, and not a failure. Nothing is staged, and the user is
// not asked to restart for no reason.
bool StageSelfUpdateFromPayload(const std::filesystem::path& payloadHub, std::string& outError);

// Performs the rename dance on an already-staged update. On success the caller should
// relaunch immediately: the process still running is the OLD binary, now living under a
// different name. Rolls the first rename back if the second fails.
bool ApplySelfUpdate(std::string& outError);

// Starts the Hub at its normal path and returns true if the new process was created. Used
// after ApplySelfUpdate; the caller then exits.
bool RelaunchHub(std::string& outError);

bool SelfUpdateSelfTest(); // part of --test-hub

} // namespace hbe::hub
