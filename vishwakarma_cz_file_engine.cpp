import subprocess, hashlib, os
from datetime import datetime, timezone, timedelta

cpp_code = r"""#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace vishwakarma::nbs {

enum class Family {
    Circular = 0,
    Linear = 1,
    Curvilinear = 2,
    Angular = 3,
    Hybrid = 4,
    Unknown = 99
};

enum Direction : unsigned {
    Vertical = 1u,
    Horizontal = 2u,
    Diagonal = 4u,
    Curved = 8u
};

struct Geometry {
    int digit = -1;
    int strokes = 0;
    int closedLoops = 0;
    int openPaths = 0;
    int junctions = 0;
    unsigned directionMask = 0;
    int topologyClass = 0;
    Family family = Family::Unknown;
};

struct SimilarityWeights {
    double strokes = 1.0 / 6.0;
    double loops = 1.0 / 6.0;
    double openPaths = 1.0 / 6.0;
    double junctions = 1.0 / 6.0;
    double direction = 1.0 / 6.0;
    double topology = 1.0 / 6.0;
};

struct Analysis {
    int firstDigit;
    int secondDigit;
    double score;
    bool sameFamily;
};

inline double countSimilarity(int a, int b) {
    const int denom = std::max(1, std::max(std::abs(a), std::abs(b)));
    return 1.0 - static_cast<double>(std::abs(a - b)) / static_cast<double>(denom);
}

inline double directionSimilarity(unsigned a, unsigned b) {
    if (a == 0 && b == 0) return 1.0;
    const unsigned intersection = a & b;
    const unsigned unionBits = a | b;
    int common = 0, total = 0;
    for (unsigned bit = Vertical; bit <= Curved; bit <<= 1) {
        if (intersection & bit) ++common;
        if (unionBits & bit) ++total;
    }
    return total == 0 ? 0.0 : static_cast<double>(common) / static_cast<double>(total);
}

inline double topologySimilarity(int a, int b) {
    return a == b ? 1.0 : 0.0;
}

inline double similarity(const Geometry& a, const Geometry& b,
                         const SimilarityWeights& w = {}) {
    const double totalWeight = w.strokes + w.loops + w.openPaths +
                               w.junctions + w.direction + w.topology;
    if (totalWeight <= 0.0) return 0.0;
    const double score =
        w.strokes * countSimilarity(a.strokes, b.strokes) +
        w.loops * countSimilarity(a.closedLoops, b.closedLoops) +
        w.openPaths * countSimilarity(a.openPaths, b.openPaths) +
        w.junctions * countSimilarity(a.junctions, b.junctions) +
        w.direction * directionSimilarity(a.directionMask, b.directionMask) +
        w.topology * topologySimilarity(a.topologyClass, b.topologyClass);
    return std::clamp(score / totalWeight, 0.0, 1.0);
}

inline const std::array<Geometry, 10>& baselineGeometry() {
    static const std::array<Geometry, 10> data{{
        {0, 1, 1, 0, 0, Curved, 1, Family::Circular},
        {1, 1, 0, 1, 0, Vertical, 2, Family::Linear},
        {2, 1, 0, 1, 0, Curved | Diagonal, 3, Family::Curvilinear},
        {3, 1, 0, 2, 0, Curved, 3, Family::Curvilinear},
        {4, 3, 0, 3, 1, Vertical | Horizontal | Diagonal, 4, Family::Angular},
        {5, 2, 0, 2, 0, Horizontal | Diagonal | Curved, 5, Family::Hybrid},
        {6, 1, 1, 1, 0, Curved, 6, Family::Circular},
        {7, 2, 0, 2, 0, Horizontal | Diagonal, 4, Family::Angular},
        {8, 1, 2, 0, 0, Curved, 7, Family::Circular},
        {9, 1, 1, 1, 0, Curved, 6, Family::Circular}
    }};
    return data;
}

inline Geometry digitGeometry(int digit) {
    if (digit < 0 || digit > 9) {
        throw std::out_of_range("Digits must be in the range 0..9.");
    }
    return baselineGeometry()[static_cast<std::size_t>(digit)];
}

inline Analysis compare(int first, int second,
                        const SimilarityWeights& weights = {}) {
    if (first < 0 || first > 9 || second < 0 || second > 9) {
        throw std::out_of_range("Digits must be in the range 0..9.");
    }
    const auto& g = baselineGeometry();
    return Analysis{
        first,
        second,
        similarity(g[static_cast<std::size_t>(first)],
                   g[static_cast<std::size_t>(second)],
                   weights),
        g[static_cast<std::size_t>(first)].family ==
            g[static_cast<std::size_t>(second)].family
    };
}

using StreamId = std::uint32_t;
using LaneId   = std::uint16_t;

struct Frame {
    StreamId stream{};
    LaneId lane{};
    Family family{Family::Unknown};
    std::uint64_t seq{};
    std::vector<std::uint8_t> payload;
};

struct Route {
    StreamId stream;
    LaneId lane;
    Family family;
};

class IPBR {
    std::map<StreamId, Route> byStream;
    std::map<LaneId, StreamId> byLane;
    std::map<StreamId, std::uint64_t> txSeq, rxSeq;
    std::map<StreamId, std::vector<std::uint8_t>> received;

public:
    void registerStream(StreamId s, LaneId l, Family f) {
        if (byStream.count(s) || byLane.count(l)) {
            throw std::runtime_error("duplicate stream/lane");
        }
        byStream[s] = {s, l, f};
        byLane[l] = s;
        txSeq[s] = 0;
        rxSeq[s] = 0;
    }

    Frame transmit(StreamId s, const std::vector<std::uint8_t>& bytes) {
        auto i = byStream.find(s);
        if (i == byStream.end()) {
            throw std::runtime_error("unknown stream");
        }
        return {s, i->second.lane, i->second.family, txSeq[s]++, bytes};
    }

    void receive(const Frame& f) {
        auto l = byLane.find(f.lane);
        if (l == byLane.end() || l->second != f.stream) {
            throw std::runtime_error("identity/lane mismatch");
        }
        auto r = byStream.find(f.stream);
        if (r == byStream.end() || r->second.family != f.family) {
            throw std::runtime_error("family/context mismatch");
        }
        if (f.seq != rxSeq[f.stream]) {
            throw std::runtime_error("sequence mismatch");
        }
        ++rxSeq[f.stream];
        received[f.stream] = f.payload;
    }

    const std::vector<std::uint8_t>& data(StreamId s) const {
        auto i = received.find(s);
        if (i == received.end()) {
            throw std::runtime_error("no data");
        }
        return i->second;
    }
};

} // namespace vishwakarma::nbs

namespace vcore {

using i64 = std::int64_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u16 = std::uint16_t;
using u8  = std::uint8_t;

enum class Transition : std::uint8_t { Sequential, CZ, Cross, DPZ, Pi, NanoBytes, ViorBinary };
enum class DPZType : std::uint8_t { Normal, Manifest, Latent, Field, Primed, Absorbed, Origin };

struct DPZValue {
    i64 scalar = 0;
    i64 latentMemory = 0;
    i64 fieldSource = 0;
    i64 primedValue = 0;
    i64 absorbedValue = 0;
    DPZType type = DPZType::Normal;
    bool isManifest = true;
    bool isZeroField = false;
};

struct PiState {
    long double theta = 0.0L;
    long double omega = 1.0L;
    long double deltaR = 1.0L;
    long double lastPhaseStep = 0.0L;
    std::uint64_t k = 0;
};

struct State {
    i64 boundedState = 0;
    std::array<int, 4> behavior{};
    std::array<i64, 4> neighbors{};
    std::array<int, 4> route{};

    i64 alpha = 1, beta = 1, gamma = 0;
    i64 plusVector = 0, minusVector = 0;
    i64 deltaState = 0, equilibriumError = 0;
    bool equilibrium = false;

    i64 energyPositive = 0, energyNegative = 0, energyZero = 0;
    bool balanced = true;

    DPZValue dpz{};
    PiState pi{};
    Transition transition = Transition::Sequential;
    u32 token = 0;

    u32 nanoSeedQ16 = 0x13570000u;
    u32 nanoScaleQ16 = 0x00A40000u;
    u32 nanoOffsetQ16 = 0x00550000u;
    std::uint64_t nanoReadCount = 0;
    std::uint64_t nanoLastIndex = 0;
    u32 nanoLastChecksum = 0;
};

class BlackBox {
private:
    static constexpr i64 MOD = 10;
    static constexpr u32 TOKEN_MIX = 0xA7C39E21u;
    State s{};

    static constexpr std::array<std::array<int, 4>, 10> behaviorTable{{
        {{0,0,0,0}}, {{1,0,0,1}}, {{0,1,0,1}}, {{1,1,0,0}},
        {{0,0,1,1}}, {{1,0,1,0}}, {{0,1,1,0}}, {{1,1,1,1}},
        {{0,0,0,1}}, {{1,1,0,1}}
    }};

    static i64 mod10(i64 x) {
        i64 r = x % MOD;
        return r < 0 ? r + MOD : r;
    }

    static u32 rotl(u32 x, unsigned n) {
        n &= 31u;
        return n == 0 ? x : static_cast<u32>((x << n) | (x >> (32u - n)));
    }

    void updateUBX() {
        s.boundedState = mod10(s.boundedState);
        s.behavior = behaviorTable[static_cast<std::size_t>(s.boundedState)];
        const i64 x = s.boundedState;
        s.neighbors = {{mod10(x + 1), mod10(x - 1), mod10(x + 2), mod10(x - 2)}};
        s.route = s.behavior;
    }

    void updateBalance() {
        s.energyZero = -(s.energyPositive + s.energyNegative);
        s.balanced = (s.energyPositive + s.energyNegative + s.energyZero == 0);
    }

    void finalize() {
        updateUBX();
        updateBalance();
        s.token = makeToken();
    }

    u32 makeToken() const {
        u32 x = TOKEN_MIX ^ static_cast<u32>(s.boundedState);
        x ^= static_cast<u32>(s.behavior[0] | (s.behavior[1] << 1) |
                              (s.behavior[2] << 2) | (s.behavior[3] << 3)) << 4;
        x ^= static_cast<u32>(s.pi.k);
        x = rotl(x, 7);
        x ^= static_cast<u32>(s.plusVector);
        x ^= rotl(static_cast<u32>(s.minusVector), 11);
        x ^= static_cast<u32>(s.dpz.type) << 24;
        x ^= static_cast<u32>(s.dpz.latentMemory);
        x ^= static_cast<u32>(s.dpz.fieldSource);
        x ^= static_cast<u32>(s.pi.theta);
        x ^= s.nanoSeedQ16;
        x ^= rotl(s.nanoScaleQ16, 5);
        x ^= rotl(s.nanoOffsetQ16, 17);
        x ^= static_cast<u32>(s.nanoReadCount);
        x ^= s.nanoLastChecksum;
        x *= 0x9E3779B1u;
        return rotl(x, 13);
    }

public:
    BlackBox() { reset(); }

    void reset() {
        s = State{};
        s.pi.theta = 0.0L;
        s.pi.omega = 1.0L;
        s.pi.deltaR = 1.0L;
        s.dpz.type = DPZType::Origin;
        s.dpz.scalar = 0;
        s.dpz.isManifest = true;
        finalize();
    }

    i64 czProcess(i64 plusInput, i64 minusInput) {
        const i64 old = s.boundedState;
        s.plusVector = plusInput * s.alpha;
        s.minusVector = minusInput * s.beta;
        const i64 decay = s.gamma * old;
        s.boundedState = mod10(old + s.plusVector - s.minusVector - decay);
        s.deltaState = s.boundedState - old;
        s.equilibriumError = s.plusVector - s.minusVector;
        s.equilibrium = (s.equilibriumError == 0);
        s.transition = Transition::Cross;
        finalize();
        return s.boundedState;
    }

    void setCZCoefficients(i64 alpha, i64 beta, i64 gamma) {
        s.alpha = alpha < 0 ? 0 : alpha;
        s.beta = beta < 0 ? 0 : beta;
        s.gamma = gamma < 0 ? 0 : gamma;
        finalize();
    }

    i64 applyBinaryBehavior(int bit) {
        const int b = (bit != 0) ? 1 : 0;
        const i64 prevState = s.boundedState;
        const int routed = s.behavior[static_cast<std::size_t>(prevState % 4)];

        if (b == 1) {
            s.plusVector = 1 + routed;
            s.minusVector = 0;
            s.boundedState = mod10(prevState + s.plusVector * s.alpha - s.gamma * prevState);
        } else {
            s.plusVector = 0;
            s.minusVector = 1 + (1 - routed);
            s.boundedState = mod10(prevState - s.minusVector * s.beta - s.gamma * prevState);
        }

        s.deltaState = s.boundedState - prevState;
        s.equilibriumError = s.plusVector - s.minusVector;
        s.equilibrium = (s.equilibriumError == 0);
        s.transition = Transition::ViorBinary;
        finalize();
        return s.boundedState;
    }

    const State& inspect() const { return s; }
    u32 token() const { return s.token; }
};

namespace vme {

inline std::string currentTimestampUTC() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tmUtc{};
    gmtime_r(&secs, &tmUtc);

    std::ostringstream oss;
    oss << std::put_time(&tmUtc, "%Y-%m-%d %H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count() << " UTC";
    return oss.str();
}

inline void advanceCZGeometryState(BlackBox& box, u8 byte) {
    const i64 plusIn = static_cast<i64>((byte >> 4) & 0x0fu);
    const i64 minusIn = static_cast<i64>(byte & 0x0fu);
    const i64 stVal = box.czProcess(plusIn, minusIn);

    for (int bitPos = 7; bitPos >= 0; --bitPos) {
        box.applyBinaryBehavior((byte >> bitPos) & 1);
    }

    const auto& st = box.inspect();
    const int d1 = static_cast<int>(std::llabs(stVal) % 10);
    const int d2 = static_cast<int>((static_cast<unsigned>(byte) +
                                     static_cast<unsigned>(st.boundedState)) % 10u);
    (void)vishwakarma::nbs::digitGeometry(d1);
    (void)vishwakarma::nbs::compare(d1, d2);
}

class CZStateReducer {
    static int czPredictBias(const BlackBox& box, u8 prev) {
        const auto& st = box.inspect();
        const int bMask = (st.behavior[0] << 3) | (st.behavior[1] << 2) |
                          (st.behavior[2] << 1) | st.behavior[3];
        const int shift = static_cast<int>((st.boundedState + bMask) % 5) - 2;
        int pred = static_cast<int>(prev) + shift;
        return std::clamp(pred, 0, 255);
    }

    static bool findBestWindowMatch(const std::vector<u8>& input,
                                    std::size_t pos,
                                    std::size_t& bestDist,
                                    std::size_t& bestLen) {
        const std::size_t n = input.size();
        if (pos + 4 > n || pos == 0) return false;

        const std::size_t maxWindow = std::min<std::size_t>(pos, 65535);
        const std::size_t maxLen = std::min<std::size_t>(35, n - pos);

        bestDist = 0;
        bestLen = 0;

        const std::size_t startBack = pos - maxWindow;
        for (std::size_t cand = pos; cand-- > startBack; ) {
            if (input[cand] != input[pos] || input[cand + 1] != input[pos + 1] ||
                input[cand + 2] != input[pos + 2] || input[cand + 3] != input[pos + 3]) {
                continue;
            }
            std::size_t len = 4;
            while (len < maxLen && input[cand + len] == input[pos + len]) {
                ++len;
            }
            if (len > bestLen) {
                bestLen = len;
                bestDist = pos - cand;
                if (bestLen == maxLen) break;
            }
        }
        return bestLen >= 4;
    }

public:
    static std::vector<u8> compress(const std::vector<u8>& input) {
        std::vector<u8> out;
        if (input.empty()) return out;

        BlackBox box;
        box.reset();
        box.setCZCoefficients(1, 1, 0);

        out.push_back(1u);
        const std::size_t n = input.size();
        std::size_t i = 0;
        u8 prev = 0;
        bool hasPrev = false;

        while (i < n) {
            const u8 curr = input[i];

            std::size_t runLen = 1;
            while (i + runLen < n && input[i + runLen] == curr && runLen < 66) {
                ++runLen;
            }
            if (runLen >= 3) {
                const u8 hdr = static_cast<u8>(0x80u | static_cast<u8>(runLen - 3));
                out.push_back(hdr);
                out.push_back(curr);
                for (std::size_t k = 0; k < runLen; ++k) {
                    advanceCZGeometryState(box, curr);
                }
                prev = curr;
                hasPrev = true;
                i += runLen;
                continue;
            }

            std::size_t bestDist = 0, bestLen = 0;
            if (findBestWindowMatch(input, i, bestDist, bestLen)) {
                const u8 hdr = static_cast<u8>(0xC0u | static_cast<u8>(bestLen - 4));
                const u8 dHi = static_cast<u8>((bestDist >> 8) & 0xFFu);
                const u8 dLo = static_cast<u8>(bestDist & 0xFFu);
                out.push_back(hdr);
                out.push_back(dHi);
                out.push_back(dLo);
                for (std::size_t k = 0; k < bestLen; ++k) {
                    const u8 b = input[i + k];
                    advanceCZGeometryState(box, b);
                    prev = b;
                }
                hasPrev = true;
                i += bestLen;
                continue;
            }

            if (hasPrev) {
                const int pred = czPredictBias(box, prev);
                const int diff = static_cast<int>(curr) - pred;
                if (diff >= -64 && diff <= 63) {
                    out.push_back(static_cast<u8>(diff + 64));
                    advanceCZGeometryState(box, curr);
                    prev = curr;
                    ++i;
                    continue;
                }
            }

            out.push_back(0xE0u);
            out.push_back(curr);
            advanceCZGeometryState(box, curr);
            prev = curr;
            hasPrev = true;
            ++i;
        }

        if (out.size() >= input.size() + 1) {
            std::vector<u8> rawStream;
            rawStream.reserve(input.size() + 1);
            rawStream.push_back(0u);
            rawStream.insert(rawStream.end(), input.begin(), input.end());
            return rawStream;
        }
        return out;
    }

    // Direct CZ pattern-based reader: reads logical bytes/patterns on-the-fly
    // without restoring or writing a full uncompressed file back to disk.
    static void readDirectlyByCZPattern(const std::vector<u8>& encoded,
                                        std::size_t previewLimit,
                                        const std::string& patternFilter,
                                        u64& totalLogicalBytesRead,
                                        std::size_t& patternMatchesFound,
                                        std::string& previewSnippet,
                                        BlackBox& outBox) {
        totalLogicalBytesRead = 0;
        patternMatchesFound = 0;
        previewSnippet.clear();
        outBox.reset();
        outBox.setCZCoefficients(1, 1, 0);

        if (encoded.empty()) return;

        const u8 mode = encoded[0];
        std::vector<u8> ringHistory; // small rolling history only for back-reference reads
        ringHistory.reserve(65536);

        auto emitLogicalByte = [&](u8 b) {
            advanceCZGeometryState(outBox, b);
            ++totalLogicalBytesRead;
            if (previewSnippet.size() < previewLimit) {
                previewSnippet.push_back(static_cast<char>(b));
            }
            if (ringHistory.size() == 65536) {
                ringHistory.erase(ringHistory.begin());
            }
            ringHistory.push_back(b);
        };

        if (mode == 0u) {
            for (std::size_t i = 1; i < encoded.size(); ++i) {
                emitLogicalByte(encoded[i]);
            }
            if (!patternFilter.empty() && previewSnippet.find(patternFilter) != std::string::npos) {
                ++patternMatchesFound;
            }
            return;
        }
        if (mode != 1u) {
            throw std::runtime_error("Unsupported CZ reducer mode");
        }

        std::size_t idx = 1;
        bool hasPrev = false;
        u8 prev = 0;
        std::string recentWindow;

        auto trackPattern = [&](char c) {
            if (patternFilter.empty()) return;
            recentWindow.push_back(c);
            if (recentWindow.size() > 256) {
                recentWindow.erase(recentWindow.begin(), recentWindow.begin() + 128);
            }
            if (recentWindow.size() >= patternFilter.size() &&
                recentWindow.compare(recentWindow.size() - patternFilter.size(),
                                     patternFilter.size(), patternFilter) == 0) {
                ++patternMatchesFound;
            }
        };

        while (idx < encoded.size()) {
            const u8 tok = encoded[idx++];
            if ((tok & 0x80u) == 0u) {
                if (!hasPrev) throw std::runtime_error("Delta token without previous state");
                const int pred = czPredictBias(outBox, prev);
                const int diff = static_cast<int>(tok) - 64;
                const int val = pred + diff;
                if (val < 0 || val > 255) throw std::runtime_error("CZ delta out of range");
                const u8 b = static_cast<u8>(val);
                emitLogicalByte(b);
                trackPattern(static_cast<char>(b));
                prev = b;
            } else if ((tok & 0xC0u) == 0x80u) {
                if (idx >= encoded.size()) throw std::runtime_error("Truncated run token");
                const std::size_t runLen = static_cast<std::size_t>(tok & 0x3Fu) + 3;
                const u8 b = encoded[idx++];
                for (std::size_t k = 0; k < runLen; ++k) {
                    emitLogicalByte(b);
                    trackPattern(static_cast<char>(b));
                }
                prev = b;
                hasPrev = true;
            } else if ((tok & 0xE0u) == 0xC0u) {
                if (idx + 1 >= encoded.size()) throw std::runtime_error("Truncated match token");
                const std::size_t matchLen = static_cast<std::size_t>(tok & 0x1Fu) + 4;
                const std::size_t dist = (static_cast<std::size_t>(encoded[idx]) << 8) |
                                         static_cast<std::size_t>(encoded[idx + 1]);
                idx += 2;
                if (dist == 0 || dist > ringHistory.size()) {
                    throw std::runtime_error("Invalid window match distance");
                }
                const std::size_t startIdx = ringHistory.size() - dist;
                std::vector<u8> matchedSlice;
                matchedSlice.reserve(matchLen);
                for (std::size_t k = 0; k < matchLen; ++k) {
                    matchedSlice.push_back(ringHistory[startIdx + k]);
                }
                for (u8 b : matchedSlice) {
                    emitLogicalByte(b);
                    trackPattern(static_cast<char>(b));
                    prev = b;
                }
                hasPrev = true;
            } else if (tok == 0xE0u) {
                if (idx >= encoded.size()) throw std::runtime_error("Truncated literal token");
                const u8 b = encoded[idx++];
                emitLogicalByte(b);
                trackPattern(static_cast<char>(b));
                prev = b;
                hasPrev = true;
            } else {
                throw std::runtime_error("Unknown CZ token");
            }
        }
    }
};

class StreamCipher {
public:
    static std::vector<u8> encryptAES256CTR(const std::vector<u8>& plain,
                                            const std::array<u8, 32>& key) {
        std::array<u8, 16> iv{};
        if (RAND_bytes(iv.data(), static_cast<int>(iv.size())) != 1) {
            throw std::runtime_error("Failed to generate random IV");
        }

        std::vector<u8> out;
        out.resize(16 + plain.size());
        std::copy(iv.begin(), iv.end(), out.begin());

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

        int len = 0, totalLen = 0;
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, key.data(), iv.data()) != 1 ||
            (!plain.empty() && EVP_EncryptUpdate(ctx, out.data() + 16, &len,
                                                 plain.data(), static_cast<int>(plain.size())) != 1)) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("AES-256-CTR encryption failed");
        }
        totalLen += len;
        if (EVP_EncryptFinal_ex(ctx, out.data() + 16 + totalLen, &len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("AES-256-CTR encrypt final failed");
        }
        EVP_CIPHER_CTX_free(ctx);
        return out;
    }

    static std::vector<u8> decryptAES256CTR(const std::vector<u8>& cipherWithIV,
                                            const std::array<u8, 32>& key) {
        if (cipherWithIV.size() < 16) {
            throw std::runtime_error("Ciphertext too short for IV");
        }
        std::array<u8, 16> iv{};
        std::copy(cipherWithIV.begin(), cipherWithIV.begin() + 16, iv.begin());

        const std::size_t cipherLen = cipherWithIV.size() - 16;
        std::vector<u8> plain(cipherLen);

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

        int len = 0, totalLen = 0;
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, key.data(), iv.data()) != 1 ||
            (cipherLen > 0 && EVP_DecryptUpdate(ctx, plain.data(), &len,
                                                cipherWithIV.data() + 16,
                                                static_cast<int>(cipherLen)) != 1)) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("AES-256-CTR decryption failed");
        }
        totalLen += len;
        if (EVP_DecryptFinal_ex(ctx, plain.data() + totalLen, &len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("AES-256-CTR decrypt final failed");
        }
        EVP_CIPHER_CTX_free(ctx);
        return plain;
    }

    static std::array<u8, 32> deriveKeyFromPassphrase(const std::string& pass) {
        std::array<u8, 32> key{};
        unsigned int outLen = 0;
        EVP_Digest(pass.data(), pass.size(), key.data(), &outLen, EVP_sha256(), nullptr);
        return key;
    }
};

class DirectCZPatternPipeline {
    static constexpr vishwakarma::nbs::StreamId STREAM_ID = 1001;
    static constexpr vishwakarma::nbs::LaneId   LANE_ID   = 10;
    static constexpr vishwakarma::nbs::Family   FAMILY_ID = vishwakarma::nbs::Family::Circular;

public:
    static bool sendReducedFile(const std::string& inputPath,
                                const std::string& outFramePath,
                                const std::string& passphrase) {
        std::ifstream fin(inputPath, std::ios::binary | std::ios::ate);
        if (!fin) {
            std::cerr << "Failed to open input file: " << inputPath << "\n";
            return false;
        }
        const u64 origBytes = static_cast<u64>(fin.tellg());
        fin.seekg(0, std::ios::beg);

        std::vector<u8> rawInput(static_cast<std::size_t>(origBytes));
        if (origBytes > 0 &&
            !fin.read(reinterpret_cast<char*>(rawInput.data()), static_cast<std::streamsize>(origBytes))) {
            std::cerr << "Failed to read input file\n";
            return false;
        }

        std::vector<u8> czReduced = CZStateReducer::compress(rawInput);

        BlackBox txBox;
        txBox.reset();
        txBox.setCZCoefficients(1, 1, 0);
        for (u8 b : rawInput) {
            advanceCZGeometryState(txBox, b);
        }

        const auto key = StreamCipher::deriveKeyFromPassphrase(passphrase);
        std::vector<u8> encryptedPayload = StreamCipher::encryptAES256CTR(czReduced, key);

        vishwakarma::nbs::IPBR router;
        router.registerStream(STREAM_ID, LANE_ID, FAMILY_ID);
        vishwakarma::nbs::Frame frame = router.transmit(STREAM_ID, encryptedPayload);

        std::ofstream fout(outFramePath, std::ios::binary | std::ios::trunc);
        if (!fout) {
            std::cerr << "Failed to open output frame file: " << outFramePath << "\n";
            return false;
        }

        fout.write("IPBR", 4);
        u32 sid = frame.stream;
        u16 lid = frame.lane;
        u8  fid = static_cast<u8>(frame.family);
        u64 seq = frame.seq;
        u64 psz = static_cast<u64>(frame.payload.size());
        fout.write(reinterpret_cast<const char*>(&sid), sizeof(sid));
        fout.write(reinterpret_cast<const char*>(&lid), sizeof(lid));
        fout.write(reinterpret_cast<const char*>(&fid), sizeof(fid));
        fout.write(reinterpret_cast<const char*>(&seq), sizeof(seq));
        fout.write(reinterpret_cast<const char*>(&psz), sizeof(psz));
        if (!frame.payload.empty()) {
            fout.write(reinterpret_cast<const char*>(frame.payload.data()),
                       static_cast<std::streamsize>(frame.payload.size()));
        }
        fout.flush();

        const auto& st = txBox.inspect();
        const double savedPct = origBytes > 0
            ? (1.0 - static_cast<double>(encryptedPayload.size()) / static_cast<double>(origBytes)) * 100.0
            : 0.0;

        std::cout << "=== Compact CZ Send (No Full Unpack Required at Receiver) ===\n"
                  << "Input File           : " << inputPath << "\n"
                  << "Original File Size   : " << origBytes << " bytes\n"
                  << "CZ Reduced Size      : " << czReduced.size() << " bytes\n"
                  << "Encrypted Payload    : " << encryptedPayload.size() << " bytes ("
                  << std::fixed << std::setprecision(2) << savedPct << "% smaller)\n"
                  << "Final CZ State       : " << st.boundedState << "\n"
                  << "Final Token          : 0x" << std::hex << std::setw(8) << std::setfill('0')
                  << st.token << std::dec << "\n";
        return true;
    }

    static bool readReducedDirectly(const std::string& inFramePath,
                                  const std::string& passphrase,
                                  const std::string& patternFilter) {
        std::ifstream fin(inFramePath, std::ios::binary | std::ios::ate);
        if (!fin) {
            std::cerr << "Failed to open IPBR frame file: " << inFramePath << "\n";
            return false;
        }
        const u64 frameDiskBytes = static_cast<u64>(fin.tellg());
        fin.seekg(0, std::ios::beg);

        char magic[4]{};
        u32 sid = 0;
        u16 lid = 0;
        u8  fid = 0;
        u64 seq = 0, psz = 0;

        if (!fin.read(magic, 4) || std::string(magic, 4) != "IPBR" ||
            !fin.read(reinterpret_cast<char*>(&sid), sizeof(sid)) ||
            !fin.read(reinterpret_cast<char*>(&lid), sizeof(lid)) ||
            !fin.read(reinterpret_cast<char*>(&fid), sizeof(fid)) ||
            !fin.read(reinterpret_cast<char*>(&seq), sizeof(seq)) ||
            !fin.read(reinterpret_cast<char*>(&psz), sizeof(psz))) {
            std::cerr << "Invalid or truncated IPBR frame header\n";
            return false;
        }

        std::vector<u8> encryptedPayload(static_cast<std::size_t>(psz));
        if (psz > 0 &&
            !fin.read(reinterpret_cast<char*>(encryptedPayload.data()), static_cast<std::streamsize>(psz))) {
            std::cerr << "Truncated IPBR frame payload\n";
            return false;
        }

        const std::string rxTimeUTC = currentTimestampUTC();

        vishwakarma::nbs::IPBR router;
        router.registerStream(STREAM_ID, LANE_ID, FAMILY_ID);
        vishwakarma::nbs::Frame frame{
            sid, lid, static_cast<vishwakarma::nbs::Family>(fid), seq, std::move(encryptedPayload)
        };
        router.receive(frame);

        const auto key = StreamCipher::deriveKeyFromPassphrase(passphrase);
        std::vector<u8> czReduced = StreamCipher::decryptAES256CTR(router.data(STREAM_ID), key);

        u64 logicalBytesRead = 0;
        std::size_t patternMatches = 0;
        std::string previewSnippet;
        BlackBox rxBox;
        CZStateReducer::readDirectlyByCZPattern(
            czReduced, 120, patternFilter, logicalBytesRead, patternMatches, previewSnippet, rxBox);

        const auto& st = rxBox.inspect();
        std::cout << "=== Direct CZ Pattern Read (Full File Not Restored to Disk) ===\n"
                  << "Receiver Time (UTC)  : " << rxTimeUTC << "\n"
                  << "Verified Stream      : " << sid << " on Lane " << lid << "\n"
                  << "Active Frame on Disk : " << frameDiskBytes << " bytes (compact)\n"
                  << "Decrypted CZ Stream  : " << czReduced.size() << " bytes\n"
                  << "Logical Bytes Read   : " << logicalBytesRead << " bytes via CZ pattern state\n"
                  << "Pattern Searched     : \"" << patternFilter << "\" (matches: " << patternMatches << ")\n"
                  << "Direct Read Preview  : \"" << previewSnippet << "\"\n"
                  << "Final CZ State       : " << st.boundedState << "\n"
                  << "Final Token          : 0x" << std::hex << std::setw(8) << std::setfill('0')
                  << st.token << std::dec << "\n";
        return true;
    }
};

} // namespace vme
} // namespace vcore

int main(int argc, char** argv) {
    using namespace vcore;
    if (argc == 5 && std::string(argv[1]) == "--cz-send") {
        return vme::DirectCZPatternPipeline::sendReducedFile(argv[2], argv[3], argv[4]) ? 0 : 1;
    }
    if (argc == 5 && std::string(argv[1]) == "--cz-read") {
        return vme::DirectCZPatternPipeline::readReducedDirectly(argv[2], argv[3], argv[4]) ? 0 : 1;
    }
    std::cerr << "Usage:\n"
              << "  " << argv[0] << " --cz-send INPUT_FILE OUT_FRAME PASSPHRASE\n"
              << "  " << argv[0] << " --cz-read OUT_FRAME PASSPHRASE PATTERN_FILTER\n";
    return 2;
}
"""

with open("vishwakarma_direct_cz_read.cpp", "w", encoding="utf-8") as f:
    f.write(cpp_code)

comp = subprocess.run(
    ["g++", "-std=c++17", "-O3", "vishwakarma_direct_cz_read.cpp", "-lcrypto", "-o", "cz_direct_read"],
    capture_output=True, text=True
)
print("COMPILE RC:", comp.returncode)
if comp.stderr:
    print("G++ STDERR:\n", comp.stderr)
assert comp.returncode == 0, "Compilation failed!"

REAL_FILE_PATH = "real_sample_log.txt"
if not os.path.exists(REAL_FILE_PATH):
    with open(REAL_FILE_PATH, "wb") as f:
        for idx in range(500):
            f.write(f"[2026-11-28] NODE=Vcore-CZ LANE=10 SEQ={idx} STATUS=OK\n".encode("utf-8"))

orig_sz = os.path.getsize(REAL_FILE_PATH)
secret_key = "JangidGroup-Vishwakarma-VCore-2026"
compact_frame = "compact_stream.ipbr"
pattern_to_check = "STATUS=OK"

s_res = subprocess.run(
    ["./cz_direct_read", "--cz-send", REAL_FILE_PATH, compact_frame, secret_key],
    capture_output=True, text=True, check=True
)
r_res = subprocess.run(
    ["./cz_direct_read", "--cz-read", compact_frame, secret_key, pattern_to_check],
    capture_output=True, text=True, check=True
)

ist_now = datetime.now(timezone(timedelta(hours=5, minutes=30))).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3] + " IST"

print(s_res.stdout)
print(r_res.stdout)
print(f"Receiver Time (IST)  : {ist_now}")
print(f"Original File Size   : {orig_sz} bytes")
print(f"Compact Frame Size   : {os.path.getsize(compact_frame)} bytes (stays compact on receiver)")
