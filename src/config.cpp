// config.cpp — config loading + validation (port of config.ts semantics, simplified).
#include "config.h"
#include "jsonc.h"
#include "resources.h"
#include "util.h"
#include <fstream>

// The default config is a faithful port of upstream dsh-pet `assets/config.jsonc`
// (v0.1.7). It lives in assets/default-config.jsonc and is EMBEDDED into the exe as
// RCDATA "CFG" by the build pipeline (tools/rcgen.py). An external
// dsh-pet-standalone.jsonc beside the exe replaces it entirely.
//
// kFallbackConfig below is only reached when the RCDATA resource is missing (a
// broken/synthetic build without windres). It is deliberately tiny so the two
// sources cannot meaningfully drift.
static const char kFallbackConfig[] =
    "{\"pets\":[{\"id\":\"main\",\"size\":350,\"position\":{\"corner\":\"top-right\","
    "\"marginX\":24,\"marginY\":100}}],"
    "\"animations\":{\"idle\":[\"待机呼吸休闲\"],\"turn\":[\"东张西望\"],"
    "\"drag\":[\"被鼠标拖拽悬空反馈\"],\"clicks\":[\"点击回应-开心跃动\"],"
    "\"moves\":{\"default\":{},\"actions\":[{\"name\":\"螃蟹走路\"}]},\"categories\":[]},"
    "\"animationWeights\":{\"idle\":10,\"turn\":5,\"move\":5}}";

static bool parseAnimConfig(const Json& root, AppConfig& cfg, std::string* err) {
    const Json* pets = root.get("pets");
    if (!pets || !pets->isArr() || pets->arr.empty()) {
        if (err) *err = "缺少 pets 或 pets 为空";
        return false;
    }
    for (const auto& p : pets->arr) {
        PetCfg pet;
        pet.id = p.asStr("main");
        if (pet.id.empty()) pet.id = "main";
        pet.size = p.asNum(350);
        const Json* pos = p.get("position");
        if (pos) {
            pet.pos.corner = pos->asStr("top-right");
            pet.pos.marginX = pos->asNum(24);
            pet.pos.marginY = pos->asNum(100);
        }
        cfg.pets.push_back(pet);
    }

    const Json* a = root.get("animations");
    if (!a || !a->isObj()) {
        if (err) *err = "缺少 animations";
        return false;
    }
    auto strVec = [](const Json* j) {
        std::vector<std::string> v;
        if (j && j->isArr())
            for (const auto& e : j->arr) v.push_back(e.asStr());
        return v;
    };
    cfg.animations.idle = strVec(a->get("idle"));
    cfg.animations.turn = strVec(a->get("turn"));
    cfg.animations.drag = strVec(a->get("drag"));
    cfg.animations.clicks = strVec(a->get("clicks"));

    const Json* mv = a->get("moves");
    if (mv && mv->isObj()) {
        const Json* def = mv->get("default");
        if (def) {
            cfg.animations.moves.minDist = def->asNum(cfg.animations.moves.minDist);
            cfg.animations.moves.maxDist = def->asNum(cfg.animations.moves.maxDist);
            cfg.animations.moves.margin = def->asNum(cfg.animations.moves.margin);
            cfg.animations.moves.leadSec = def->asNum(cfg.animations.moves.leadSec);
            cfg.animations.moves.tailSec = def->asNum(cfg.animations.moves.tailSec);
        }
        const Json* acts = mv->get("actions");
        if (acts && acts->isArr()) {
            for (const auto& e : acts->arr) {
                MoveActionCfg ma;
                ma.name = e.asStr();
                const Json* params = e.get("params");
                if (params) {
                    ma.minDist = params->asNum(-1);
                    ma.maxDist = params->asNum(-1);
                    ma.margin = params->asNum(-1);
                    ma.leadSec = params->asNum(-1);
                    ma.tailSec = params->asNum(-1);
                }
                cfg.animations.moves.actions.push_back(ma);
            }
        }
    }

    const Json* cats = a->get("categories");
    if (cats && cats->isArr()) {
        for (const auto& c : cats->arr) {
            CategoryCfg cat;
            auto idF = c.get("id");
            if (idF) cat.id = idF->asStr();
            auto wF = c.get("weight");
            if (wF) cat.weight = wF->asNum(0);
            auto nmF = c.get("noMirror");
            if (nmF) cat.noMirror = nmF->asBool(false);
            const Json* acts = c.get("actions");
            if (acts && acts->isArr())
                for (const auto& e : acts->arr) cat.actions.push_back(e.asStr());
            cfg.animations.categories.push_back(std::move(cat));
        }
    }

    const Json* w = root.get("animationWeights");
    if (w && w->isObj()) {
        cfg.weights.idle = w->asNum(10);
        cfg.weights.turn = w->asNum(5);
        cfg.weights.move = w->asNum(5);
    }
    return true;
}

bool loadConfig(const std::wstring& exeDir, const std::wstring& configPath, AppConfig& out, std::string* err) {
    (void)exeDir;
    std::string text;
    bool external = false;
    if (!configPath.empty()) {
        // libstdc++ has no wstring path ctor; pass the raw wide C string.
        std::ifstream f(configPath.c_str(), std::ios::binary);
        if (f) {
            text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            external = true;
        }
    }
    if (!external) {
        text = loadEmbeddedConfigText();  // RCDATA "CFG" (default config, size 350)
        if (text.empty()) text = kFallbackConfig;  // only for broken builds without resources
    }
    Json root = parseJsonc(text);
    if (root.type != Json::Type::Obj) {
        if (err) *err = external ? "外部配置文件解析失败（非 JSON/JSONC 对象）" : "内置默认配置解析失败";
        return false;
    }
    AppConfig cfg;
    if (!parseAnimConfig(root, cfg, err)) return false;
    out = std::move(cfg);
    return true;
}