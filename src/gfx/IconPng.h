#pragma once
// Validation and decoding of icon PNGs (Increment 11). The bytes come from the network or from the SD card, so they are
// untrusted: the size is capped, and the file is walked chunk by chunk BEFORE any decoder sees the data, because a
// small file can be a decompression bomb in two ways:
//   - a huge declared size (60000 x 60000): refused from the IHDR alone;
//   - a tiny image whose IDAT inflates to hundreds of MB, which libpng inflates and discards, taking seconds on a Wii
//     inside one non-sliceable worker step: refused by comparing the IDAT size with what the declared image can need,
//     and then by inflating the IDAT ourselves into a counter that stops at the exact raw size.
// libpng only ever sees a stripped copy (IHDR, PLTE, tRNS, IDAT, IEND), so a compressed iCCP / zTXt / iTXt chunk, the
// other inflate bomb, is never decompressed at all. The decoded surface is checked against the header too.
//
// Thread-safe: touches no global state besides SDL_image's own, so the network worker decodes here and only the
// finished SDL_Surface (CPU memory) goes to the main thread, which alone creates textures.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

struct SDL_Surface;

namespace iconpng {

// The largest icon file accepted, from the network or the SD card (real icons are 2 to 14 KB).
inline constexpr size_t kMaxFileBytes = 64 * 1024;
// Most chunks a file may have. A real icon has 5 to 15; the limit only stops a file of 8-byte chunks from costing a long walk.
inline constexpr size_t kMaxChunks = 1000;
// The largest width and height accepted. The API's icons are 128x48; 256 leaves room without letting one icon cost more
// than 256 KB of RGBA. (The Wii GX texture limit is 1024.)
inline constexpr int kMaxDim = 256;

enum class Verdict { Ok, Empty, TooBig, NotPng, BadHeader, BadDimensions, BadStructure, TooMuchData, BadData, DecodeFailed };
const char* name(Verdict v);

// Checks size, signature, IHDR and the chunk structure (IHDR first, IDATs consecutive and not larger than the declared
// image can need, IEND last, nothing after it, at most kMaxChunks), and returns the declared size. Nothing is
// decompressed, so this is fast for any input. CRCs are not checked here: libpng verifies them for the critical chunks,
// and this walk only has to bound the work.
Verdict checkHeader(std::string_view bytes, int& w, int& h);

// The most IDAT bytes an image of this size and format can legitimately have, 0 if the parameters are not a valid PNG
// format. Public so the tests can build files right at the limit.
uint64_t maxIdatBytes(uint32_t w, uint32_t h, int bitDepth, int colorType, bool interlaced);
// The exact size of the image's inflated scanlines (filter bytes included; all 7 passes for Adam7).
uint64_t rawImageBytes(uint32_t w, uint32_t h, int bitDepth, int colorType, bool interlaced);

struct SurfaceDeleter { void operator()(SDL_Surface* s) const; };
using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

// checkHeader, a bounded inflate of the IDAT that must produce exactly the declared raw size, then a full decode into a 32-bit RGBA surface whose size must match the header. Any worker or main thread.
Verdict decode(std::string_view bytes, SurfacePtr& out);

}  // namespace iconpng
