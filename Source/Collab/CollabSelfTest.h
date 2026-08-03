// Collab/CollabSelfTest.h - the entry point for `--test-collab`.
//
// Declared in its own header rather than in CollabServer.h so main_editor.cpp does not
// have to include the server (and therefore the whole protocol) just to run the test.
#pragma once

namespace hbe::collab {

// Drives the shipping server and client over the loopback transport. Returns false on
// the first failed invariant, having printed each. Headless: no GPU, no window, no
// project, no sockets.
bool CollabSelfTest();

} // namespace hbe::collab
