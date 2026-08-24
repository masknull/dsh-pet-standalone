// resources.h — Win32 RCDATA resource access for the embedded default config,
// embedded-manifest and the embedded animation packs.
#pragma once
#include <string>
#include <vector>
#include "anim.h"

// Read whole RCDATA resource by (string) name into out. Returns false when absent.
bool loadResourceBytes(const wchar_t* name, std::vector<uint8_t>& out);

// Zero-copy resource view (valid for the process lifetime). v7 webm mode uses
// this so the raw VP9-alpha streams are decoded straight from the PE image.
bool loadResourcePtr(const wchar_t* name, const uint8_t** out, size_t* len);

// Embedded default config JSONC text (resource "CFG"). Empty when resource missing.
std::string loadEmbeddedConfigText();

// Embedded animation packs listed by resource "EMB" (embedded-manifest.json), each
// pack loaded from "PACK000".."PACK00N" in manifest order. Empty when none embedded.
std::vector<LoadedAnim> loadEmbeddedAnims();