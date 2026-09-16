#pragma once

// Command line.
//
// Every switch the game understands lives here as a STRUCT FIELD, not as a
// getenv() call scattered through the renderer: the command line is the
// interface, and the header is the list of what exists. The historical VV_*
// environment variables still work (scripts, CI and the owner's on-device A/B
// sweeps set them), but they only SEED the struct now - a flag always wins over
// the variable it replaces.
//
// Flags are parsed at startup in main(), into one process-wide GameOptions.
// That single instance is why the consumers deep in the renderer (the swapchain's
// present mode, the instance's validation layer) read `options()` instead of
// taking the struct through five layers of constructors - the same reach the
// environment variables had, minus the string parsing at every use.
//
// Parsing is deliberately dependency-free (no getopt) and pure: tests drive it
// with a vector of strings, without a window or a Vulkan device.

#include <string>
#include <vector>

namespace vv::core {

struct GameOptions final {
  // --help / -h: print the usage and exit without touching a window.
  bool help = false;

  // Ambient sky visibility (pass 62; new options are FLAGS ONLY - the VV_*
  // fallback exists for the switches that predate the command line, and the
  // complaint this pass answered was exactly "stop making me use variables").
  bool ambient = true;             // --no-ambient: the pre-62 ambient formula
  bool ambientFloorSet = false;
  float ambientFloor = 0.0f;       // --ambient-floor <0..1> (sky fraction)

  // Rendering experiments.
  bool sdfShadows = false;    // --sdf-shadows
  bool shadowSharp = false;   // --shadow-sharp (wins over --sdf-shadows)
  bool farLod = false;        // --far-lod
  bool shadowJitterSet = false;
  float shadowJitter = 0.0f;  // --shadow-jitter <slope> (0 = off)
  bool sdfMarginSet = false;
  int sdfMargin = 1;          // --sdf-margin <chunks>

  // Runtime.
  bool validation = false;    // --validation
  bool perf = false;          // --perf
  std::string present;        // --present <immediate|mailbox|fifo> ("" = default)
  std::string platform;       // --platform <auto|x11|wayland|null|cocoa|win32>

  // Debug views.
  bool debugTerm = false;     // --debug-term
  std::string debugHole;      // --debug-hole <chunkX,chunkZ>
};

struct CommandLineResult final {
  GameOptions options;
  std::string error;  // non-empty: the caller reports it on stderr and exits

  bool ok() const { return error.empty(); }
};

// The options as the environment describes them (VV_* variables, the same
// tolerances as before: "1"/"true"/"on"/"yes" for a flag, VV_PERF != "0",
// VV_DEBUG_TERM set at all, VV_DEBUG_HOLE "x,z"). Absent variables leave the
// defaults alone.
GameOptions optionsFromEnvironment();

// `arguments` excludes argv[0]. Environment first, then the flags on top, so a
// flag overrides the variable with the same meaning (and the LAST flag wins
// when one is repeated). Unknown flags, missing values and malformed values are
// reported through `error`, never ignored: a typo must not silently run the
// game with different settings than the user asked for.
CommandLineResult parseCommandLine(const std::vector<std::string>& arguments);

// The --help text: every flag with its default, one per line. Kept next to the
// parser so a new flag cannot be added without the help mentioning it (the
// tests check a few names, but the discipline is the real contract).
std::string commandLineUsage();

// A one-line summary of the settings that differ from the defaults, for the
// startup log ("[vv] options: ..."), or an empty string when everything is at
// its default. This is what a bug report needs: the switches actually in force.
std::string describeOptions(const GameOptions& options);

// The process-wide instance, set once by main() before anything reads it.
void setOptions(const GameOptions& options);
const GameOptions& options();

} // namespace vv::core
