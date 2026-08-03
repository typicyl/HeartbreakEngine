// Hub/HubSelfTest.cpp - `--test-hub`.
//
// Runs every part of the launcher that can be proven WITHOUT a network: version
// ordering, manifest parsing (including the exact shape published at
// hollowdreamstudios.com), the https-only URL policy, the zip-slip containment guard,
// the update state machine's refusals, and SHA-256 against published test vectors.
//
// The network itself is deliberately not exercised here - a self-test that fails when
// the office wifi drops teaches people to ignore it.
#include "Hub/HubConfig.h"
#include "Hub/ProjectCatalog.h"
#include "Hub/HubJoin.h"
#include "Hub/UpdateCheck.h"
#include "Hub/Updater.h"
#include "Hub/ZipArchive.h"

#include <cstdio>

namespace hbe::hub {

bool HubSelfTest() {
    // The Hub's own join flow - it links the collaboration client so someone with
    // nothing can fetch a project before any engine or project exists.
    bool joinOk = HubJoinSelfTest();

    // `&` not `&&`: every section must run and print, so one failure does not hide the
    // rest behind a short circuit.
    const bool a = UpdateCheckSelfTest();
    const bool b = ZipSelfTest();
    const bool c = UpdaterSelfTest();
    const bool d = ProjectCatalogSelfTest();
    const bool ok = a && b && c && d;
    if (ok) {
        std::printf("hub: version ordering (1.0.9 < 1.0.10), the published manifest shape "
                    "parses, http/@-authority/CRLF URLs refused, zip traversal + device "
                    "names + NUL refused while normal paths pass, Apply refuses without a "
                    "manifest, SHA-256 matches its test vectors, the project catalog de-duplicates and caps, a corrupt version stamp reads as UNKNOWN\n");
    }
    return (ok) && joinOk;
}

} // namespace hbe::hub
