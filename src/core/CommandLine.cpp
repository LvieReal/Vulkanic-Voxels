#include "core/CommandLine.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vv::core {

namespace {

// The tolerant "is this variable asking for something" test the renderer used
// before the command line existed; kept so every existing VV_* value in a
// script or a shell history behaves exactly as before.
bool envFlagEnabled(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return false;
  }
  return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
         std::strcmp(value, "on") == 0 || std::strcmp(value, "yes") == 0;
}

bool parseFloat(const std::string& text, float& out) {
  if (text.empty()) {
    return false;
  }
  char* end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0') {
    return false;
  }
  out = value;
  return true;
}

bool parseInt(const std::string& text, int& out) {
  if (text.empty()) {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0') {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

const char* const kPresentModes[] = {"immediate", "mailbox", "fifo"};
const char* const kPlatforms[] = {"auto",        "x11",   "wayland",
                                  "null",        "cocoa", "win32"};

bool oneOf(const std::string& value, const char* const* allowed,
           std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    if (value == allowed[i]) {
      return true;
    }
  }
  return false;
}

std::string joinList(const char* const* items, std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) {
      out += ", ";
    }
    out += items[i];
  }
  return out;
}

} // namespace

GameOptions optionsFromEnvironment() {
  GameOptions o;
  o.sdfShadows = envFlagEnabled("VV_SDF_SHADOWS");
  o.shadowSharp = envFlagEnabled("VV_SHADOW_SHARP");
  o.farLod = envFlagEnabled("VV_FAR_LOD");
  if (const char* value = std::getenv("VV_SHADOW_JITTER")) {
    float slope = 0.0f;
    if (parseFloat(value, slope)) {
      o.shadowJitterSet = true;
      o.shadowJitter = slope;
    }
  }
  if (const char* value = std::getenv("VV_SDF_MARGIN")) {
    int margin = 1;
    if (parseInt(value, margin)) {
      o.sdfMarginSet = true;
      o.sdfMargin = margin;
    }
  }
  o.validation = std::getenv("VV_VALIDATION") != nullptr;
  if (const char* value = std::getenv("VV_PERF")) {
    o.perf = std::strcmp(value, "0") != 0;
  }
  o.debugTerm = std::getenv("VV_DEBUG_TERM") != nullptr;
  if (const char* value = std::getenv("VV_DEBUG_HOLE")) {
    o.debugHole = value;
  }
  if (const char* value = std::getenv("VV_PRESENT")) {
    o.present = value;
  }
  if (const char* value = std::getenv("VV_PLATFORM")) {
    o.platform = value;
  }
  return o;
}

CommandLineResult parseCommandLine(const std::vector<std::string>& arguments) {
  CommandLineResult result;
  GameOptions& o = result.options;
  o = optionsFromEnvironment();

  // `--flag value` pairs, checked before the flag itself is matched so a value
  // that happens to spell another flag is not eaten by the wrong switch.
  const auto valueFor = [&arguments](std::size_t& i, std::string& out,
                                     std::string& error) {
    if (i + 1 >= arguments.size()) {
      error = "option '" + arguments[i] + "' needs a value";
      return false;
    }
    out = arguments[++i];
    return true;
  };

  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string& arg = arguments[i];

    if (arg == "--help" || arg == "-h") {
      o.help = true;
    } else if (arg == "--sdf-shadows") {
      o.sdfShadows = true;
    } else if (arg == "--no-sdf-shadows") {
      o.sdfShadows = false;
    } else if (arg == "--shadow-sharp") {
      o.shadowSharp = true;
    } else if (arg == "--no-shadow-sharp") {
      o.shadowSharp = false;
    } else if (arg == "--ambient") {
      o.ambient = true;
    } else if (arg == "--no-ambient") {
      o.ambient = false;
    } else if (arg == "--ambient-floor") {
      std::string value;
      if (!valueFor(i, value, result.error)) {
        return result;
      }
      float floor = 0.0f;
      if (!parseFloat(value, floor)) {
        result.error = "option '--ambient-floor' needs a number, got '" +
                       value + "'";
        return result;
      }
      o.ambientFloorSet = true;
      o.ambientFloor = std::clamp(floor, 0.0f, 1.0f);
    } else if (arg == "--far-lod") {
      o.farLod = true;
    } else if (arg == "--no-far-lod") {
      o.farLod = false;
    } else if (arg == "--validation") {
      o.validation = true;
    } else if (arg == "--no-validation") {
      o.validation = false;
    } else if (arg == "--perf") {
      o.perf = true;
    } else if (arg == "--no-perf") {
      o.perf = false;
    } else if (arg == "--debug-term") {
      o.debugTerm = true;
    } else if (arg == "--no-debug-term") {
      o.debugTerm = false;
    } else if (arg == "--shadow-jitter") {
      std::string value;
      if (!valueFor(i, value, result.error)) {
        return result;
      }
      float slope = 0.0f;
      if (!parseFloat(value, slope)) {
        result.error = "option '--shadow-jitter' needs a number, got '" + value +
                       "'";
        return result;
      }
      o.shadowJitterSet = true;
      o.shadowJitter = slope;
    } else if (arg == "--sdf-margin") {
      std::string value;
      if (!valueFor(i, value, result.error)) {
        return result;
      }
      int margin = 1;
      if (!parseInt(value, margin)) {
        result.error = "option '--sdf-margin' needs a whole number of chunks, "
                       "got '" +
                       value + "'";
        return result;
      }
      o.sdfMarginSet = true;
      o.sdfMargin = margin;
    } else if (arg == "--present") {
      std::string value;
      if (!valueFor(i, value, result.error)) {
        return result;
      }
      // "uncapped" is what the default already is (immediate when the surface
      // has it); accepting the word keeps it out of the error message a user
      // would otherwise get for describing the default.
      if (value == "uncapped") {
        value = "immediate";
      }
      if (!oneOf(value, kPresentModes,
                 sizeof(kPresentModes) / sizeof(kPresentModes[0]))) {
        result.error = "option '--present' expects one of " +
                       joinList(kPresentModes,
                                sizeof(kPresentModes) / sizeof(kPresentModes[0])) +
                       " (or 'uncapped'), got '" + value + "'";
        return result;
      }
      o.present = value;
    } else if (arg == "--platform") {
      std::string value;
      if (!valueFor(i, value, result.error)) {
        return result;
      }
      if (!oneOf(value, kPlatforms,
                 sizeof(kPlatforms) / sizeof(kPlatforms[0]))) {
        result.error = "option '--platform' expects one of " +
                       joinList(kPlatforms,
                                sizeof(kPlatforms) / sizeof(kPlatforms[0])) +
                       ", got '" + value + "'";
        return result;
      }
      o.platform = value;
    } else if (arg == "--debug-hole") {
      std::string value;
      if (!valueFor(i, value, result.error)) {
        return result;
      }
      int x = 0;
      int z = 0;
      if (std::sscanf(value.c_str(), "%d,%d", &x, &z) != 2) {
        result.error =
            "option '--debug-hole' expects chunk coordinates as x,z, got '" +
            value + "'";
        return result;
      }
      o.debugHole = value;
    } else {
      result.error = "unknown option '" + arg + "'";
      return result;
    }

    if (!result.error.empty()) {
      return result;
    }
  }

  return result;
}

std::string commandLineUsage() {
  return
      "Vulkanic Voxels - ray-traced voxel terrain\n"
      "\n"
      "Usage: game [options]\n"
      "\n"
      "Options:\n"
      "  -h, --help                 print this and exit\n"
      "  --sdf-shadows              soft shadows from the 3D voxel SDF\n"
      "                             (experiment; default: exact binary sun)\n"
      "  --shadow-sharp             force the exact binary sun shadows\n"
      "  --shadow-jitter <slope>    per-pixel shadow-ray jitter cone slope,\n"
      "                             0 turns it off (default 0.002)\n"
      "  --sdf-margin <chunks>      how far the camera may drift before the\n"
      "                             SDF field is rebuilt (default 1)\n"
      "  --no-ambient               the pre-62 ambient formula (the sky as the\n"
      "                             camera ray sees it); the default is the sky\n"
      "                             the SURFACE sees, so caves go dark\n"
      "  --ambient-floor <0..1>     how much sky a fully occluded point still\n"
      "                             gets, as a fraction (default 0.12)\n"
      "  --far-lod                  enable the coarse far-terrain LOD field\n"
      "  --validation               enable the Khronos validation layer\n"
      "  --perf                     log slow frames and the SDF bake cost\n"
      "  --present <mode>           immediate (uncapped, the default),\n"
      "                             mailbox or fifo (vsync)\n"
      "  --platform <name>          auto (default), x11, wayland, null, cocoa\n"
      "                             or win32 - forces the window platform\n"
      "  --debug-term               colour pixels by why the ray ended\n"
      "  --debug-hole <x,z>         far-LOD diagnostics over chunk (x,z)\n"
      "\n"
      "Every flag also accepts a --no- prefix where that makes sense\n"
      "(--no-far-lod), and the last one on the command line wins. The VV_*\n"
      "environment variables of the same names still work as a fallback\n"
      "(a flag overrides its variable). Run with --platform null for a\n"
      "headless smoke run.\n";
}

std::string describeOptions(const GameOptions& o) {
  std::string out;
  const auto add = [&out](const std::string& item) {
    if (!out.empty()) {
      out += ", ";
    }
    out += item;
  };
  if (!o.ambient) {
    add("no-ambient");
  }
  if (o.ambientFloorSet) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "ambient-floor %.3f",
                  static_cast<double>(o.ambientFloor));
    add(buffer);
  }
  if (o.sdfShadows) {
    add("sdf-shadows");
  }
  if (o.shadowSharp) {
    add("shadow-sharp");
  }
  if (o.shadowJitterSet) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "shadow-jitter %.3f",
                  static_cast<double>(o.shadowJitter));
    add(buffer);
  }
  if (o.sdfMarginSet) {
    add("sdf-margin " + std::to_string(o.sdfMargin));
  }
  if (o.farLod) {
    add("far-lod");
  }
  if (o.validation) {
    add("validation");
  }
  if (o.perf) {
    add("perf");
  }
  if (!o.present.empty()) {
    add("present " + o.present);
  }
  if (!o.platform.empty()) {
    add("platform " + o.platform);
  }
  if (o.debugTerm) {
    add("debug-term");
  }
  if (!o.debugHole.empty()) {
    add("debug-hole " + o.debugHole);
  }
  return out;
}

namespace {
GameOptions& instance() {
  static GameOptions g_options;
  return g_options;
}
} // namespace

void setOptions(const GameOptions& options) { instance() = options; }

const GameOptions& options() { return instance(); }

} // namespace vv::core
