#include "xb_format.hpp"

#include <algorithm>
#include <cstring>
#include <random>

namespace xb_format {
namespace {

constexpr uint8_t kMagic0 = 'X';
constexpr uint8_t kMagic1 = 'B';
constexpr uint8_t kVersionNoVelocity = 0x01;
constexpr uint8_t kVersionWithVelocity = 0x02;
constexpr uint8_t kVersionSealed = 0x03;
constexpr uint8_t kCurrentVersion = kVersionSealed;
constexpr size_t kHeaderSize = 3;
constexpr size_t kNonceSize = 8;
constexpr size_t kChecksumSize = 4;
constexpr uint32_t kMaxRecordCount = 50'000'000u;

uint32_t fnv1a(std::span<uint8_t const> data) {
    uint32_t hash = 0x811c9dc5u;
    for (uint8_t byte : data) {
        hash ^= byte;
        hash *= 0x01000193u;
    }
    return hash;
}

void writeU8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void writeU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void writeU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
}

void writeU64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
}

void writeF32(std::vector<uint8_t>& out, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32(out, bits);
}

void writeString(std::vector<uint8_t>& out, std::string const& value) {
    uint16_t len = static_cast<uint16_t>(std::min<size_t>(value.size(), 0xFFFFu));
    writeU16(out, len);
    out.insert(out.end(), value.begin(), value.begin() + len);
}

void writeVarUint(std::vector<uint8_t>& out, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7Fu);
        value >>= 7;
        if (value != 0)
            byte |= 0x80u;
        out.push_back(byte);
    } while (value != 0);
}

bool readU8(std::span<uint8_t const> data, size_t& offset, uint8_t& out) {
    if (offset + 1 > data.size())
        return false;
    out = data[offset];
    offset += 1;
    return true;
}

bool readU16(std::span<uint8_t const> data, size_t& offset, uint16_t& out) {
    if (offset + 2 > data.size())
        return false;
    out = static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
    offset += 2;
    return true;
}

bool readU32(std::span<uint8_t const> data, size_t& offset, uint32_t& out) {
    if (offset + 4 > data.size())
        return false;
    out = static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
          (static_cast<uint32_t>(data[offset + 2]) << 16) |
          (static_cast<uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return true;
}

bool readU64(std::span<uint8_t const> data, size_t& offset, uint64_t& out) {
    if (offset + 8 > data.size())
        return false;
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(data[offset + i]) << (8 * i);
    out = value;
    offset += 8;
    return true;
}

bool readF32(std::span<uint8_t const> data, size_t& offset, float& out) {
    uint32_t bits = 0;
    if (!readU32(data, offset, bits))
        return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

bool readString(std::span<uint8_t const> data, size_t& offset, std::string& out) {
    uint16_t len = 0;
    if (!readU16(data, offset, len))
        return false;
    if (offset + len > data.size())
        return false;
    out.assign(reinterpret_cast<char const*>(data.data() + offset), len);
    offset += len;
    return true;
}

bool readVarUint(std::span<uint8_t const> data, size_t& offset, uint64_t& out) {
    uint64_t result = 0;
    int shift = 0;
    while (true) {
        if (offset >= data.size())
            return false;
        uint8_t byte = data[offset++];
        result |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0)
            break;
        shift += 7;
        if (shift >= 64)
            return false;
    }
    out = result;
    return true;
}

void writeInputRecord(std::vector<uint8_t>& out, uint64_t delta, uint8_t button, bool player2,
                       bool down) {
    uint64_t deltaLow = delta & 0x7u;
    uint64_t rest = delta >> 3;

    uint8_t byte0 = static_cast<uint8_t>(deltaLow);
    byte0 |= static_cast<uint8_t>((button & 0x3u) << 5);
    if (player2)
        byte0 |= 0x10u;
    if (down)
        byte0 |= 0x08u;
    if (rest != 0)
        byte0 |= 0x80u;
    out.push_back(byte0);

    if (rest != 0)
        writeVarUint(out, rest);
}

bool readInputRecord(std::span<uint8_t const> data, size_t& offset, uint64_t& delta,
                      uint8_t& button, bool& player2, bool& down) {
    uint8_t byte0 = 0;
    if (!readU8(data, offset, byte0))
        return false;

    uint64_t deltaLow = byte0 & 0x7u;
    button = (byte0 >> 5) & 0x3u;
    player2 = (byte0 & 0x10u) != 0;
    down = (byte0 & 0x08u) != 0;

    uint64_t rest = 0;
    if (byte0 & 0x80u) {
        if (!readVarUint(data, offset, rest))
            return false;
    }

    delta = (rest << 3) | deltaLow;
    return true;
}

void writeFrameFixHeader(std::vector<uint8_t>& out, uint64_t delta, bool p1Rotate,
                          bool p2Rotate) {
    uint64_t deltaLow = delta & 0x1Fu;
    uint64_t rest = delta >> 5;

    uint8_t byte0 = static_cast<uint8_t>(deltaLow);
    if (p1Rotate)
        byte0 |= 0x40u;
    if (p2Rotate)
        byte0 |= 0x20u;
    if (rest != 0)
        byte0 |= 0x80u;
    out.push_back(byte0);

    if (rest != 0)
        writeVarUint(out, rest);
}

bool readFrameFixHeader(std::span<uint8_t const> data, size_t& offset, uint64_t& delta,
                         bool& p1Rotate, bool& p2Rotate) {
    uint8_t byte0 = 0;
    if (!readU8(data, offset, byte0))
        return false;

    uint64_t deltaLow = byte0 & 0x1Fu;
    p1Rotate = (byte0 & 0x40u) != 0;
    p2Rotate = (byte0 & 0x20u) != 0;

    uint64_t rest = 0;
    if (byte0 & 0x80u) {
        if (!readVarUint(data, offset, rest))
            return false;
    }

    delta = (rest << 5) | deltaLow;
    return true;
}

constexpr uint64_t kKeyPartA = 0x9F3ADB27C41E88B5ULL;
constexpr uint64_t kKeyPartB = 0x51C2F97A6E0D3B84ULL;
constexpr uint64_t kKeyPartC = 0x2D6F8B1A94C5E037ULL;

uint64_t sealKey() {
    uint64_t k = kKeyPartA ^ kKeyPartB;
    k = (k + 0x9E3779B97F4A7C15ULL) ^ kKeyPartC;
    k *= 0xBF58476D1CE4E5B9ULL;
    return k ^ (k >> 29);
}

uint64_t splitmix64Next(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Self-inverse: calling this twice with the same nonce recovers the input.
void applyKeystream(std::vector<uint8_t>& data, uint64_t nonce) {
    uint64_t state = sealKey() ^ nonce;
    size_t i = 0;
    while (i < data.size()) {
        uint64_t block = splitmix64Next(state);
        for (int b = 0; b < 8 && i < data.size(); ++b, ++i)
            data[i] ^= static_cast<uint8_t>(block >> (8 * b));
    }
}

uint64_t randomNonce() {
    static std::mt19937_64 rng{std::random_device{}()};
    return rng();
}

std::vector<uint8_t> buildPlaintextPayload(BotReplay const& replay) {
    std::vector<uint8_t> out;
    out.reserve(48 + replay.inputs.size() * 2 + replay.frameFixes.size() * 33);

    writeU32(out, static_cast<uint32_t>(std::max(replay.botInfo.version, 0)));
    writeU32(out, static_cast<uint32_t>(std::max(replay.gameVersion, 0)));
    writeU32(out, static_cast<uint32_t>(replay.levelInfo.id));
    writeF32(out, replay.framerate);
    writeF32(out, replay.duration);
    writeU64(out, static_cast<uint64_t>(replay.seed));
    writeU8(out, static_cast<uint8_t>(std::clamp(replay.coins, 0, 255)));

    uint8_t flags = 0;
    if (replay.ldm)
        flags |= 0x01u;
    writeU8(out, flags);

    writeString(out, replay.levelInfo.name);
    writeString(out, replay.author);
    writeString(out, replay.description);

    writeU32(out, static_cast<uint32_t>(replay.inputs.size()));
    uint64_t previousFrame = 0;
    for (auto const& input : replay.inputs) {
        uint64_t frame = input.frame;
        uint64_t delta = frame >= previousFrame ? frame - previousFrame : 0;
        writeInputRecord(out, delta, input.button, input.player2, input.down);
        previousFrame = frame;
    }

    writeU32(out, static_cast<uint32_t>(replay.frameFixes.size()));
    uint64_t previousFixFrame = 0;
    for (auto const& fix : replay.frameFixes) {
        uint64_t frame = static_cast<uint64_t>(std::max(fix.frame, 0));
        uint64_t delta = frame >= previousFixFrame ? frame - previousFixFrame : 0;
        writeFrameFixHeader(out, delta, fix.p1.rotate, fix.p2.rotate);
        previousFixFrame = frame;

        writeF32(out, fix.p1.pos.x);
        writeF32(out, fix.p1.pos.y);
        writeF32(out, fix.p1.rotation);
        writeF32(out, static_cast<float>(fix.p1.yVelocity));
        writeF32(out, static_cast<float>(fix.p1.xVelocity));
        writeF32(out, fix.p2.pos.x);
        writeF32(out, fix.p2.pos.y);
        writeF32(out, fix.p2.rotation);
        writeF32(out, static_cast<float>(fix.p2.yVelocity));
        writeF32(out, static_cast<float>(fix.p2.xVelocity));
    }

    writeU32(out, fnv1a(out));

    return out;
}

gdr::Result<BotReplay> parsePlaintextPayload(std::span<uint8_t const> payload, bool hasVelocity,
                                              bool packedRecords) {
    size_t offset = 0;

    uint32_t botVersion = 0;
    uint32_t gameVersion = 0;
    uint32_t levelId = 0;
    float framerate = 240.f;
    float duration = 0.f;
    uint64_t seed = 0;
    uint8_t coins = 0;
    uint8_t flags = 0;

    if (!readU32(payload, offset, botVersion) || !readU32(payload, offset, gameVersion) ||
        !readU32(payload, offset, levelId) || !readF32(payload, offset, framerate) ||
        !readF32(payload, offset, duration) || !readU64(payload, offset, seed) ||
        !readU8(payload, offset, coins) || !readU8(payload, offset, flags))
        return gdr::Err<BotReplay>("XB: truncated header");

    std::string levelName;
    std::string author;
    std::string description;
    if (!readString(payload, offset, levelName) || !readString(payload, offset, author) ||
        !readString(payload, offset, description))
        return gdr::Err<BotReplay>("XB: truncated header strings");

    BotReplay replay;
    replay.botInfo.name = "xdBot";
    replay.botInfo.version = static_cast<int>(botVersion);
    replay.gameVersion = static_cast<int>(gameVersion);
    replay.levelInfo.id = levelId;
    replay.levelInfo.name = levelName;
    replay.framerate = framerate;
    replay.duration = duration;
    replay.seed = static_cast<uintptr_t>(seed);
    replay.coins = static_cast<int>(coins);
    replay.ldm = (flags & 0x01u) != 0;
    replay.author = author;
    replay.description = description;
    replay.xdBotMacro = true;
    replay.isLegacy = false;

    uint32_t inputCount = 0;
    if (!readU32(payload, offset, inputCount))
        return gdr::Err<BotReplay>("XB: truncated input count");
    if (inputCount > kMaxRecordCount)
        return gdr::Err<BotReplay>("XB: input count out of sane bounds");

    replay.inputs.reserve(inputCount);
    uint64_t currentFrame = 0;
    for (uint32_t i = 0; i < inputCount; ++i) {
        uint64_t delta = 0;
        uint8_t button = 0;
        bool player2 = false;
        bool down = false;

        if (packedRecords) {
            if (!readInputRecord(payload, offset, delta, button, player2, down))
                return gdr::Err<BotReplay>("XB: truncated input data");
        } else {
            uint8_t packed = 0;
            if (!readVarUint(payload, offset, delta) || !readU8(payload, offset, packed))
                return gdr::Err<BotReplay>("XB: truncated input data");
            button = packed & 0x3Fu;
            player2 = (packed & 0x40u) != 0;
            down = (packed & 0x80u) != 0;
        }

        currentFrame += delta;
        replay.inputs.emplace_back(currentFrame, button, player2, down);
    }

    uint32_t frameFixCount = 0;
    if (!readU32(payload, offset, frameFixCount))
        return gdr::Err<BotReplay>("XB: truncated frame fix count");
    if (frameFixCount > kMaxRecordCount)
        return gdr::Err<BotReplay>("XB: frame fix count out of sane bounds");

    replay.frameFixes.reserve(frameFixCount);
    uint64_t currentFixFrame = 0;
    for (uint32_t i = 0; i < frameFixCount; ++i) {
        uint64_t delta = 0;
        gdr_legacy::FrameFix fix;
        bool p1Rotate = false;
        bool p2Rotate = false;

        if (packedRecords) {
            if (!readFrameFixHeader(payload, offset, delta, p1Rotate, p2Rotate))
                return gdr::Err<BotReplay>("XB: truncated frame fix data");
        } else {
            uint8_t fixFlags = 0;
            if (!readVarUint(payload, offset, delta) || !readU8(payload, offset, fixFlags))
                return gdr::Err<BotReplay>("XB: truncated frame fix data");
            p1Rotate = (fixFlags & 0x01u) != 0;
            p2Rotate = (fixFlags & 0x02u) != 0;
        }

        if (!readF32(payload, offset, fix.p1.pos.x) || !readF32(payload, offset, fix.p1.pos.y) ||
            !readF32(payload, offset, fix.p1.rotation))
            return gdr::Err<BotReplay>("XB: truncated frame fix data");

        if (hasVelocity) {
            float p1YVelocity = 0.f;
            float p1XVelocity = 0.f;
            if (!readF32(payload, offset, p1YVelocity) || !readF32(payload, offset, p1XVelocity))
                return gdr::Err<BotReplay>("XB: truncated frame fix data");
            fix.p1.yVelocity = static_cast<double>(p1YVelocity);
            fix.p1.xVelocity = static_cast<double>(p1XVelocity);
        }

        if (!readF32(payload, offset, fix.p2.pos.x) || !readF32(payload, offset, fix.p2.pos.y) ||
            !readF32(payload, offset, fix.p2.rotation))
            return gdr::Err<BotReplay>("XB: truncated frame fix data");

        if (hasVelocity) {
            float p2YVelocity = 0.f;
            float p2XVelocity = 0.f;
            if (!readF32(payload, offset, p2YVelocity) || !readF32(payload, offset, p2XVelocity))
                return gdr::Err<BotReplay>("XB: truncated frame fix data");
            fix.p2.yVelocity = static_cast<double>(p2YVelocity);
            fix.p2.xVelocity = static_cast<double>(p2XVelocity);
        }

        currentFixFrame += delta;
        fix.frame = static_cast<int>(currentFixFrame);
        fix.p1.rotate = p1Rotate;
        fix.p2.rotate = p2Rotate;

        replay.frameFixes.push_back(fix);
    }

    return gdr::Ok<BotReplay>(std::move(replay));
}

// Version 1/2 files: unencrypted, checksum is the last 4 bytes of the file.
gdr::Result<BotReplay> importUnsealed(std::span<uint8_t const> data) {
    if (data.size() < kHeaderSize + kChecksumSize)
        return gdr::Err<BotReplay>("XB file too short");

    size_t payloadSize = data.size() - kChecksumSize;

    uint32_t storedChecksum = 0;
    size_t checksumOffset = payloadSize;
    if (!readU32(data, checksumOffset, storedChecksum))
        return gdr::Err<BotReplay>("XB: truncated checksum");

    uint32_t computedChecksum = fnv1a(data.subspan(0, payloadSize));
    if (computedChecksum != storedChecksum)
        return gdr::Err<BotReplay>(
            "XB checksum mismatch - file is corrupted or not a genuine xb macro");

    bool hasVelocity = data[2] == kVersionWithVelocity;
    return parsePlaintextPayload(data.subspan(kHeaderSize, payloadSize - kHeaderSize),
                                  hasVelocity, false);
}

// Version 3 files: magic + plaintext nonce, then a sealed (encrypted)
// payload whose own last 4 bytes (once unsealed) are the checksum.
gdr::Result<BotReplay> importSealed(std::span<uint8_t const> data) {
    if (data.size() < kHeaderSize + kNonceSize + kChecksumSize)
        return gdr::Err<BotReplay>("XB file too short");

    size_t offset = kHeaderSize;
    uint64_t nonce = 0;
    if (!readU64(data, offset, nonce))
        return gdr::Err<BotReplay>("XB: truncated nonce");

    std::vector<uint8_t> payload(data.begin() + static_cast<ptrdiff_t>(offset), data.end());
    applyKeystream(payload, nonce);

    if (payload.size() < kChecksumSize)
        return gdr::Err<BotReplay>("XB file too short");

    size_t fieldsSize = payload.size() - kChecksumSize;
    uint32_t storedChecksum = 0;
    size_t checksumOffset = fieldsSize;
    if (!readU32(payload, checksumOffset, storedChecksum))
        return gdr::Err<BotReplay>("XB: truncated checksum");

    uint32_t computedChecksum = fnv1a(std::span<uint8_t const>(payload.data(), fieldsSize));
    if (computedChecksum != storedChecksum)
        return gdr::Err<BotReplay>(
            "XB checksum mismatch - file is corrupted or not a genuine xb macro");

    return parsePlaintextPayload(std::span<uint8_t const>(payload.data(), fieldsSize), true, true);
}

} // namespace

bool isXBData(std::span<uint8_t const> data) {
    return data.size() >= kHeaderSize && data[0] == kMagic0 && data[1] == kMagic1 &&
           (data[2] == kVersionNoVelocity || data[2] == kVersionWithVelocity ||
            data[2] == kVersionSealed);
}

std::vector<uint8_t> exportXB(BotReplay const& replay) {
    std::vector<uint8_t> payload = buildPlaintextPayload(replay);

    uint64_t nonce = randomNonce();
    applyKeystream(payload, nonce);

    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + kNonceSize + payload.size());
    out.push_back(kMagic0);
    out.push_back(kMagic1);
    out.push_back(kCurrentVersion);
    writeU64(out, nonce);
    out.insert(out.end(), payload.begin(), payload.end());

    return out;
}

gdr::Result<BotReplay> importXB(std::span<uint8_t const> data) {
    if (!isXBData(data))
        return gdr::Err<BotReplay>("Not an XB file");

    if (data[2] == kVersionSealed)
        return importSealed(data);

    return importUnsealed(data);
}

} // namespace xb_format
