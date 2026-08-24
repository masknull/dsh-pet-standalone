// config.h — runtime configuration (port of dsh-pet types.ts / config.jsonc).
#pragma once
#include <string>
#include <vector>

struct PositionCfg {
    std::string corner = "top-right";  // top-left | top-right | bottom-left | bottom-right
    double marginX = 24;
    double marginY = 100;
};

struct PetCfg {
    std::string id = "main";
    double size = 350;  // default display style: 350 wide, 16:9 -> 350x197
    PositionCfg pos;
};

struct MoveActionCfg {
    std::string name;
    // optional per-action overrides, empty = use defaults
    double minDist = -1, maxDist = -1, margin = -1, leadSec = -1, tailSec = -1;
};

struct MovesCfg {
    double minDist = 60, maxDist = 240, margin = 20, leadSec = 2, tailSec = 2;
    std::vector<MoveActionCfg> actions;
};

struct CategoryCfg {
    std::string id;
    double weight = 0;
    bool noMirror = false;
    std::vector<std::string> actions;
};

struct AnimationsCfg {
    std::vector<std::string> idle, turn, drag, clicks;
    MovesCfg moves;
    std::vector<CategoryCfg> categories;
};

struct WeightsCfg {
    double idle = 10, turn = 5, move = 5;
};

struct AppConfig {
    std::vector<PetCfg> pets;
    AnimationsCfg animations;
    WeightsCfg weights;
};

// Parsed from (in priority order):
//   1. <exe dir>/dsh-pet-standalone.jsonc   (full replacement, JSONC allowed)
//   2. embedded default (RCDATA "CFG")        = upstream dsh-pet assets/config.jsonc
// CLI --size and --assets are applied afterwards by the caller.
bool loadConfig(const std::wstring& exeDir, const std::wstring& configPath, AppConfig& out, std::string* err);