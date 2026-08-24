// inflate.h — minimal raw-DEFLATE (RFC 1951) decompressor, zero external dependencies.
// Decoder logic follows the classic canonical-Huffman per-bit decode approach.
#pragma once
#include <cstdint>
#include <cstddef>

// Inflate a raw DEFLATE stream (no zlib header/adler) located in [in, in+inLen).
// Writes at most expectedLen bytes into out. Returns:
//   0                    success (stream consumed, output == expectedLen checked by caller)
//   -1                   not enough input bits / truncated stream
//   -2                   invalid block header
//   -3                   invalid Huffman code / over-subscribed code lengths
//   -4                   output length mismatch (would exceed expectedLen)
// On success *actualOut = number of bytes decoded.
int inflateRaw(const uint8_t* in, size_t inLen, uint8_t* out, size_t expectedLen, size_t* actualOut);