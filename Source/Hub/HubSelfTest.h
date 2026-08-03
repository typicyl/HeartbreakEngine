// Hub/HubSelfTest.h - entry point for `--test-hub`.
#pragma once
namespace hbe::hub {
// Headless: no network, no GPU, no window. Writes only into the OS temp directory.
bool HubSelfTest();
} // namespace hbe::hub
