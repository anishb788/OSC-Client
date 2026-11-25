#include "gfx/IconPng.h"

#include <SDL_image.h>
#include <SDL_pixels.h>
#include <SDL_rwops.h>
#include <SDL_surface.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>

namespace iconpng {

const char* name(Verdict v) {
	switch (v) {
	case Verdict::Ok: return "ok";
	case Verdict::Empty: return "empty";
	case Verdict::TooBig: return "file too big";
	case Verdict::NotPng: return "not a PNG";
	case Verdict::BadHeader: return "bad PNG header";
	case Verdict::BadDimensions: return "image dimensions out of range";
	case Verdict::BadStructure: return "bad PNG chunk structure";
	case Verdict::TooMuchData: return "more image data than the size allows";
	case Verdict::BadData: return "image data does not match the header";
	case Verdict::DecodeFailed: return "PNG decode failed";
	}
	return "?";
}

void SurfaceDeleter::operator()(SDL_Surface* s) const { SDL_FreeSurface(s); }

namespace {
uint32_t be32(const unsigned char* p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Channels for a colour type, 0 when the type or its bit depth is not allowed by the PNG spec.
int channelsFor(int colorType, int bitDepth) {
	switch (colorType) {
	case 0: return (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8 || bitDepth == 16) ? 1 : 0;
	case 2: return (bitDepth == 8 || bitDepth == 16) ? 3 : 0;
	case 3: return (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8) ? 1 : 0;
	case 4: return (bitDepth == 8 || bitDepth == 16) ? 2 : 0;
	case 6: return (bitDepth == 8 || bitDepth == 16) ? 4 : 0;
	}
	return 0;
}

// Scanline bytes and rows of the inflated data: every row is one filter byte plus ceil(width * bits per pixel / 8).
// Adam7 stores 7 reduced images instead; a pass with no pixels has no rows at all.
struct Layout {
	uint64_t raw = 0, rows = 0;
};
Layout layoutOf(uint32_t w, uint32_t h, int bpp, bool interlaced) {
	Layout l;
	auto add = [&](uint64_t pw, uint64_t ph) {
		if (!pw || !ph) return;
		l.raw += ph * (1 + (pw * uint64_t(bpp) + 7) / 8);
		l.rows += ph;
	};
	if (!interlaced) {
		add(w, h);
		return l;
	}
	static const unsigned xs[7] = {0, 4, 0, 2, 0, 1, 0}, ys[7] = {0, 0, 4, 0, 2, 0, 1};
	static const unsigned dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
	for (int i = 0; i < 7; i++) add(w > xs[i] ? (w - xs[i] + dx[i] - 1) / dx[i] : 0, h > ys[i] ? (h - ys[i] + dy[i] - 1) / dy[i] : 0);
	return l;
}

// Why no legitimate encoder can exceed this: a zlib stream is never larger than the same data in stored blocks, which
// cost 5 bytes per 65535 bytes of data, plus the 2 byte header and the 4 byte adler32. An encoder that also flushes
// after EVERY scanline (a streaming writer's Z_SYNC_FLUSH) can add, per row, one more stored block header (5 bytes) and
// the empty block the flush emits (5 bytes): 10 bytes per row at the very most (the tests build exactly that worst case
// at level 0, and the bound holds with room). 64 bytes are left over. Real encoders land within 0.1% of the raw size.
// This only bounds the IDAT length, which is not the inflated size (a few KB of IDAT can inflate to 1000x): the exact
// size is enforced by verifyIdat() below.
uint64_t idatBound(const Layout& l) { return l.raw + 10 * l.rows + 5 * (l.raw / 65535 + 1) + 6 + 64; }

struct Chunk {
	char type[4];
	size_t whole;     // offset of the length field
	size_t dataLen;
};

struct Parsed {
	uint32_t w = 0, h = 0;
	int bitDepth = 0, colorType = 0;
	bool interlaced = false;
	Layout layout;
	std::vector<Chunk> chunks;   // in file order, at most kMaxChunks
};

bool isType(const Chunk& c, const char* t) { return std::memcmp(c.type, t, 4) == 0; }

// The structural walk behind checkHeader and decode. No decompression; the work is linear in a file of at most
// kMaxFileBytes and at most kMaxChunks chunks.
Verdict parse(std::string_view bytes, Parsed& out) {
	if (bytes.empty()) return Verdict::Empty;
	if (bytes.size() > kMaxFileBytes) return Verdict::TooBig;
	static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	// signature (8) + IHDR chunk: length (4), type (4), 13 data bytes
	if (bytes.size() < 8 + 8 + 13) return std::memcmp(bytes.data(), sig, std::min<size_t>(bytes.size(), 8)) == 0 ? Verdict::BadHeader : Verdict::NotPng;
	const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data());
	if (std::memcmp(p, sig, 8) != 0) return Verdict::NotPng;
	if (be32(p + 8) != 13 || std::memcmp(p + 12, "IHDR", 4) != 0) return Verdict::BadHeader;
	out.w = be32(p + 16);
	out.h = be32(p + 20);
	if (out.w == 0 || out.h == 0 || out.w > uint32_t(kMaxDim) || out.h > uint32_t(kMaxDim)) return Verdict::BadDimensions;
	out.bitDepth = p[24];
	out.colorType = p[25];
	out.interlaced = p[28] == 1;
	const int channels = channelsFor(out.colorType, out.bitDepth);
	if (!channels || p[26] != 0 || p[27] != 0 || p[28] > 1) return Verdict::BadHeader;
	out.layout = layoutOf(out.w, out.h, channels * out.bitDepth, out.interlaced);

	const uint64_t bound = idatBound(out.layout);
	uint64_t idatTotal = 0;
	enum class Phase { BeforeIdat, InIdat, AfterIdat, AfterEnd } phase = Phase::BeforeIdat;
	size_t pos = 8;
	while (pos < bytes.size()) {
		if (phase == Phase::AfterEnd) return Verdict::BadStructure;   // data after IEND
		if (out.chunks.size() >= kMaxChunks) return Verdict::BadStructure;
		if (bytes.size() - pos < 12) return Verdict::BadStructure;    // a cut-off chunk header or CRC
		const uint64_t len = be32(p + pos);
		if (len > bytes.size() - pos - 12) return Verdict::BadStructure;   // runs past the end of the file
		Chunk c;
		std::memcpy(c.type, p + pos + 4, 4);
		c.whole = pos;
		c.dataLen = size_t(len);
		const bool first = out.chunks.empty();
		if (isType(c, "IHDR")) {
			if (!first) return Verdict::BadStructure;   // a second IHDR
		} else if (first) {
			return Verdict::BadHeader;
		}
		if (isType(c, "IDAT")) {
			if (phase == Phase::AfterIdat) return Verdict::BadStructure;   // IDATs must be consecutive
			phase = Phase::InIdat;
			idatTotal += len;
			if (idatTotal > bound) return Verdict::TooMuchData;
		} else {
			if (phase == Phase::InIdat) phase = Phase::AfterIdat;
			if (isType(c, "PLTE") && phase != Phase::BeforeIdat) return Verdict::BadStructure;
			if (isType(c, "IEND")) {
				if (len != 0 || phase == Phase::BeforeIdat) return Verdict::BadStructure;   // no image data at all
				phase = Phase::AfterEnd;
			}
		}
		out.chunks.push_back(c);
		pos += 12 + size_t(len);
	}
	if (phase != Phase::AfterEnd) return Verdict::BadStructure;   // no IEND: truncated
	return Verdict::Ok;
}

// Inflates the IDAT chunks into a scratch buffer that is thrown away, only counting. Stops as soon as the output
// exceeds the exact size the header implies, so a stream cannot cost more than that plus one buffer, whatever it claims.
Verdict verifyIdat(std::string_view bytes, const Parsed& pr) {
	z_stream zs;
	std::memset(&zs, 0, sizeof zs);
	if (inflateInit(&zs) != Z_OK) return Verdict::DecodeFailed;
	unsigned char scratch[4096];
	uint64_t total = 0;
	int rc = Z_OK;
	for (const Chunk& c : pr.chunks) {
		if (!isType(c, "IDAT")) continue;
		zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data())) + c.whole + 8;
		zs.avail_in = uInt(c.dataLen);
		do {
			zs.next_out = scratch;
			zs.avail_out = sizeof scratch;
			rc = inflate(&zs, Z_NO_FLUSH);
			total += sizeof scratch - zs.avail_out;
			if (total > pr.layout.raw) {
				inflateEnd(&zs);
				return Verdict::TooMuchData;
			}
			if (rc == Z_BUF_ERROR) rc = Z_OK;   // this chunk's input is used up and nothing was produced: next chunk
			if (rc != Z_OK) break;
		} while (zs.avail_out == 0);   // a full buffer means output may still be pending
		if (rc != Z_OK) break;
	}
	inflateEnd(&zs);
	return (rc == Z_STREAM_END && total == pr.layout.raw) ? Verdict::Ok : Verdict::BadData;
}

// The file reduced to what the image needs. Every dropped chunk is ancillary (text, colour profile, gamma, time, APNG
// animation data), none changes the pixels libpng returns, and the compressed ones (iCCP, zTXt, iTXt) are exactly where
// a second kind of inflate bomb would hide. Chunks are copied verbatim, CRCs included, so libpng still verifies them.
std::string stripped(std::string_view bytes, const Parsed& pr) {
	std::string out(bytes.substr(0, 8));
	for (const Chunk& c : pr.chunks) {
		if (isType(c, "IHDR") || isType(c, "PLTE") || isType(c, "tRNS") || isType(c, "IDAT") || isType(c, "IEND"))
			out.append(bytes.substr(c.whole, c.dataLen + 12));
	}
	return out;
}
}  // namespace

uint64_t rawImageBytes(uint32_t w, uint32_t h, int bitDepth, int colorType, bool interlaced) {
	const int ch = channelsFor(colorType, bitDepth);
	return ch ? layoutOf(w, h, ch * bitDepth, interlaced).raw : 0;
}

uint64_t maxIdatBytes(uint32_t w, uint32_t h, int bitDepth, int colorType, bool interlaced) {
	const int ch = channelsFor(colorType, bitDepth);
	return ch ? idatBound(layoutOf(w, h, ch * bitDepth, interlaced)) : 0;
}

Verdict checkHeader(std::string_view bytes, int& w, int& h) {
	w = h = 0;
	Parsed pr;
	const Verdict v = parse(bytes, pr);
	if (v == Verdict::Ok || v == Verdict::BadStructure || v == Verdict::TooMuchData) {
		w = int(pr.w);
		h = int(pr.h);
	}
	return v;
}

Verdict decode(std::string_view bytes, SurfacePtr& out) {
	out.reset();
	Parsed pr;
	if (Verdict v = parse(bytes, pr); v != Verdict::Ok) return v;
	if (Verdict v = verifyIdat(bytes, pr); v != Verdict::Ok) return v;
	const std::string clean = stripped(bytes, pr);
	SDL_RWops* rw = SDL_RWFromConstMem(clean.data(), int(clean.size()));
	if (!rw) return Verdict::DecodeFailed;
	SurfacePtr raw(IMG_LoadPNG_RW(rw));
	SDL_RWclose(rw);
	if (!raw || raw->w != int(pr.w) || raw->h != int(pr.h)) return Verdict::DecodeFailed;
	// One known layout for the main thread's upload, converted here so that work stays off the render thread.
	if (raw->format->format != SDL_PIXELFORMAT_RGBA32) {
		SurfacePtr conv(SDL_ConvertSurfaceFormat(raw.get(), SDL_PIXELFORMAT_RGBA32, 0));
		if (!conv) return Verdict::DecodeFailed;
		raw = std::move(conv);
	}
	out = std::move(raw);
	return Verdict::Ok;
}

}  // namespace iconpng
