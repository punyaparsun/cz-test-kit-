#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vcore {

using i64 = std::int64_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using u8  = std::uint8_t;

enum class Transition : std::uint8_t { Sequential, CZ, Cross, DPZ, Pi, NanoBytes, ViorBinary };
enum class DPZType : std::uint8_t { Normal, Manifest, Latent, Field, Primed, Absorbed, Origin };

struct DPZValue {
    i64 scalar = 0;
    i64 latentMemory = 0;  // Px
    i64 fieldSource = 0;   // Phi_x
    i64 primedValue = 0;   // x*
    i64 absorbedValue = 0; // x-bar
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

    // Nano-Bytes deterministic virtual-data state (Q16.16 parameters).
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
    static constexpr std::uint64_t VIRTUAL_CAPACITY = 2048ULL * 512ULL;
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

    void applyDPZ(const DPZValue& v) {
        s.dpz = v;
        s.transition = Transition::DPZ;
        if (v.isManifest) s.boundedState = mod10(v.scalar);
        finalize();
    }

    u8 nanoByteAt(std::uint64_t virtualIndex) const {
        const u32 index32 = static_cast<u32>(virtualIndex);
        u32 computed = s.nanoSeedQ16 + index32 * s.nanoScaleQ16;
        computed >>= 16;
        return static_cast<u8>((computed ^ (s.nanoOffsetQ16 >> 16)) & 0xffu);
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

    void load(i64 value) {
        s.boundedState = mod10(value);
        s.transition = Transition::Sequential;
        finalize();
    }

    i64 next() {
        s.boundedState = mod10(s.boundedState + 1);
        s.transition = Transition::Sequential;
        finalize();
        return s.boundedState;
    }

    i64 same(i64 a, i64 b) {
        s.boundedState = mod10(a + b);
        s.transition = Transition::Sequential;
        finalize();
        return s.boundedState;
    }

    i64 opposite(i64 a, i64 b) {
        s.boundedState = mod10(a - b);
        s.transition = Transition::Cross;
        finalize();
        return s.boundedState;
    }

    i64 reinforce(i64 a, i64 b) {
        s.boundedState = mod10(a * b);
        s.transition = Transition::Sequential;
        finalize();
        return s.boundedState;
    }

    void setCZCoefficients(i64 alpha, i64 beta, i64 gamma) {
        s.alpha = alpha < 0 ? 0 : alpha;
        s.beta = beta < 0 ? 0 : beta;
        s.gamma = gamma < 0 ? 0 : gamma;
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

    void setEnergy(i64 positive, i64 negativeMagnitude) {
        s.energyPositive = positive;
        s.energyNegative = negativeMagnitude > 0 ? -negativeMagnitude : negativeMagnitude;
        finalize();
    }

    i64 dpzZeroTimesZero() {
        DPZValue v; v.scalar = 1; v.type = DPZType::Origin; v.isManifest = true;
        applyDPZ(v); return v.scalar;
    }

    void dpzMultiplyZero(i64 x) {
        DPZValue v; v.scalar = 0; v.latentMemory = x; v.type = DPZType::Latent;
        v.isManifest = false; v.isZeroField = true; applyDPZ(v);
    }

    void dpzDivideZero(i64 x) {
        DPZValue v; v.scalar = 0; v.fieldSource = x; v.type = DPZType::Field;
        v.isManifest = false; v.isZeroField = true; applyDPZ(v);
    }

    void dpzAddZero(i64 x) {
        DPZValue v; v.scalar = x; v.primedValue = x; v.type = DPZType::Primed;
        v.isManifest = true; applyDPZ(v);
    }

    void dpzZeroMinus(i64 x) {
        DPZValue v; v.scalar = 0; v.absorbedValue = -x; v.type = DPZType::Absorbed;
        v.isManifest = false; v.isZeroField = true; applyDPZ(v);
    }

    i64 dpzZeroPowerZero() {
        DPZValue v; v.scalar = 1; v.type = DPZType::Origin; v.isManifest = true;
        applyDPZ(v); return v.scalar;
    }

    void dpzMultiply(i64 a, i64 b) {
        if (a == 0 && b == 0) { dpzZeroTimesZero(); return; }
        if (b == 0) { dpzMultiplyZero(a); return; }
        if (a == 0) { dpzMultiplyZero(b); return; }
        DPZValue v; v.scalar = a * b; v.type = DPZType::Normal; v.isManifest = true;
        applyDPZ(v);
    }

    void dpzDivide(i64 a, i64 b) {
        if (b == 0) { dpzDivideZero(a); return; }
        DPZValue v; v.scalar = a / b; v.type = DPZType::Normal; v.isManifest = true;
        applyDPZ(v);
    }

    void dpzAdd(i64 a, i64 b) {
        if (b == 0) { dpzAddZero(a); return; }
        if (a == 0) { dpzAddZero(b); return; }
        DPZValue v; v.scalar = a + b; v.type = DPZType::Normal; v.isManifest = true;
        applyDPZ(v);
    }

    void dpzSubtract(i64 a, i64 b) {
        if (a == 0) { dpzZeroMinus(b); return; }
        DPZValue v; v.scalar = a - b; v.type = DPZType::Normal; v.isManifest = true;
        applyDPZ(v);
    }

    void configurePi(long double omega, long double radialScale) {
        s.pi.omega = omega;
        s.pi.deltaR = radialScale;
        finalize();
    }

    void piStep(long double dt) {
        constexpr long double PI_APPROX =
            3.141592653589793238462643383279502884L;

        s.pi.lastPhaseStep = PI_APPROX * s.pi.omega * dt;
        s.pi.theta += s.pi.lastPhaseStep;
        s.pi.k += 1;
        s.pi.deltaR += s.pi.omega * dt;
        s.transition = Transition::Pi;
        finalize();
    }

    void configureNanoBytes(u32 seedQ16, u32 scaleQ16, u32 offsetQ16) {
        s.nanoSeedQ16 = seedQ16;
        s.nanoScaleQ16 = scaleQ16;
        s.nanoOffsetQ16 = offsetQ16;
        s.transition = Transition::NanoBytes;
        finalize();
    }

    bool nanoRead(std::uint64_t offset, u8* output, std::size_t length) {
        if (output == nullptr || offset > VIRTUAL_CAPACITY ||
            length > VIRTUAL_CAPACITY - offset) {
            return false;
        }

        u32 checksum = 2166136261u;
        for (std::size_t i = 0; i < length; ++i) {
            const u8 byte = nanoByteAt(offset + i);
            output[i] = byte;
            checksum ^= byte;
            checksum *= 16777619u;
        }

        s.nanoLastIndex = offset;
        s.nanoLastChecksum = checksum;
        ++s.nanoReadCount;
        s.transition = Transition::NanoBytes;
        finalize();
        return true;
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

enum class Op : std::uint8_t {
    Reset=1, Load, Next, Same, Opposite, Reinforce,
    SetCZ, CZProcess, SetEnergy,
    DPZZeroTimesZero, DPZMultiplyZero, DPZDivideZero,
    DPZAddZero, DPZZeroMinus, DPZZeroPowerZero,
    DPZMultiply, DPZDivide, DPZAdd, DPZSubtract,
    ConfigurePi, PiStep, ConfigureNano, NanoRead
};

struct Command {
    std::uint32_t sequence = 0;
    Op op = Op::Reset;
    std::array<i64,4> arg{};
    std::string text;
};

struct Result {
    i64 value = 0;
    bool accepted = true;
    std::vector<u8> bytes;
};

class MorsePacket {
    static u32 hash(const std::vector<u8>& v) {
        u32 h = 2166136261u;
        for (auto b : v) { h ^= b; h *= 16777619u; }
        return h;
    }
    static void put32(std::vector<u8>& v, u32 x) {
        for (unsigned i=0;i<4;++i) v.push_back(static_cast<u8>(x>>(8*i)));
    }
    static void put64(std::vector<u8>& v, u64 x) {
        for (unsigned i=0;i<8;++i) v.push_back(static_cast<u8>(x>>(8*i)));
    }
    static u32 get32(const std::vector<u8>& v, std::size_t p) {
        if (p+4>v.size()) throw std::runtime_error("VME truncated u32");
        u32 x=0; for(unsigned i=0;i<4;++i) x |= u32(v[p+i])<<(8*i); return x;
    }
    static u64 get64(const std::vector<u8>& v, std::size_t p) {
        if (p+8>v.size()) throw std::runtime_error("VME truncated u64");
        u64 x=0; for(unsigned i=0;i<8;++i) x |= u64(v[p+i])<<(8*i); return x;
    }
public:
    static std::vector<u8> serialize(const Command& c) {
        if (c.text.size() > 65535) throw std::runtime_error("VME text field too large");
        std::vector<u8> v{'V','M','E',1};
        put32(v,c.sequence);
        v.push_back(static_cast<u8>(c.op));
        for (auto x:c.arg) put64(v,static_cast<u64>(x));
        v.push_back(static_cast<u8>(c.text.size() & 0xff));
        v.push_back(static_cast<u8>((c.text.size() >> 8) & 0xff));
        v.insert(v.end(),c.text.begin(),c.text.end());
        put32(v,hash(v));
        return v;
    }
    static std::string encode(const Command& c) {
        const auto bytes=serialize(c);
        std::ostringstream out; out << '#';
        for (std::size_t i=0;i<bytes.size();++i) {
            if(i) out << '/';
            for(int bit=7;bit>=0;--bit) out << ((bytes[i]>>bit)&1 ? '-' : '.');
        }
        out << '#'; return out.str();
    }
    static Command decode(const std::string& packet) {
        if(packet.size()<2 || packet.front()!='#' || packet.back()!='#')
            throw std::runtime_error("VME invalid packet boundary");
        std::vector<u8> v;
        std::stringstream ss(packet.substr(1,packet.size()-2));
        std::string token;
        while(std::getline(ss,token,'/')) {
            if(token.size()!=8) throw std::runtime_error("VME invalid Morse byte width");
            u8 b=0;
            for(char ch:token) {
                b=static_cast<u8>(b<<1);
                if(ch=='-') b|=1;
                else if(ch!='.') throw std::runtime_error("VME invalid Morse symbol");
            }
            v.push_back(b);
        }
        constexpr std::size_t fixed=4+4+1+32+2+4;
        if(v.size()<fixed) throw std::runtime_error("VME packet too short");
        if(v[0]!='V'||v[1]!='M'||v[2]!='E'||v[3]!=1) throw std::runtime_error("VME header/version mismatch");
        const std::uint16_t textLen=static_cast<std::uint16_t>(v[41] | (std::uint16_t(v[42])<<8));
        if(v.size()!=fixed+textLen) throw std::runtime_error("VME packet length mismatch");
        const auto expected=get32(v,v.size()-4);
        std::vector<u8> payload(v.begin(),v.end()-4);
        if(hash(payload)!=expected) throw std::runtime_error("VME checksum mismatch");
        Command c;
        c.sequence=get32(v,4);
        const auto rawOp=v[8];
        if(rawOp<static_cast<u8>(Op::Reset) || rawOp>static_cast<u8>(Op::NanoRead))
            throw std::runtime_error("VME unknown opcode");
        c.op=static_cast<Op>(rawOp);
        for(unsigned i=0;i<4;++i) c.arg[i]=static_cast<i64>(get64(v,9+8*i));
        c.text.assign(v.begin()+43,v.end()-4);
        return c;
    }
};

class ExperienceProtocol {
public:
    enum class Stage { Observe, Validate, Transform, Commit };
    struct Trace { Stage stage; std::uint32_t sequence; bool valid; };
    Trace observe(std::uint32_t n) const { return {Stage::Observe,n,true}; }
    Trace validate(std::uint32_t n,bool ok) const { return {Stage::Validate,n,ok}; }
    Trace transform(std::uint32_t n,bool ok) const { return {Stage::Transform,n,ok}; }
    Trace commit(std::uint32_t n,bool ok) const { return {Stage::Commit,n,ok}; }
};

class Runtime {
private:
    BlackBox core_;
    std::uint32_t nextSequence_=0;
    ExperienceProtocol experience_;

    static std::string realText(long double x) {
        std::ostringstream os;
        os << std::setprecision(std::numeric_limits<long double>::max_digits10) << x;
        return os.str();
    }

    Result dispatch(const Command& c) {
        Result r;
        switch(c.op) {
            case Op::Reset: core_.reset(); break;
            case Op::Load: core_.load(c.arg[0]); break;
            case Op::Next: r.value=core_.next(); break;
            case Op::Same: r.value=core_.same(c.arg[0],c.arg[1]); break;
            case Op::Opposite: r.value=core_.opposite(c.arg[0],c.arg[1]); break;
            case Op::Reinforce: r.value=core_.reinforce(c.arg[0],c.arg[1]); break;
            case Op::SetCZ: core_.setCZCoefficients(c.arg[0],c.arg[1],c.arg[2]); break;
            case Op::CZProcess: r.value=core_.czProcess(c.arg[0],c.arg[1]); break;
            case Op::SetEnergy: core_.setEnergy(c.arg[0],c.arg[1]); break;
            case Op::DPZZeroTimesZero: r.value=core_.dpzZeroTimesZero(); break;
            case Op::DPZMultiplyZero: core_.dpzMultiplyZero(c.arg[0]); break;
            case Op::DPZDivideZero: core_.dpzDivideZero(c.arg[0]); break;
            case Op::DPZAddZero: core_.dpzAddZero(c.arg[0]); break;
            case Op::DPZZeroMinus: core_.dpzZeroMinus(c.arg[0]); break;
            case Op::DPZZeroPowerZero: r.value=core_.dpzZeroPowerZero(); break;
            case Op::DPZMultiply: core_.dpzMultiply(c.arg[0],c.arg[1]); break;
            case Op::DPZDivide: core_.dpzDivide(c.arg[0],c.arg[1]); break;
            case Op::DPZAdd: core_.dpzAdd(c.arg[0],c.arg[1]); break;
            case Op::DPZSubtract: core_.dpzSubtract(c.arg[0],c.arg[1]); break;
            case Op::ConfigurePi: {
                const auto comma=c.text.find(',');
                if(comma==std::string::npos) throw std::runtime_error("VME Pi parameters missing");
                core_.configurePi(std::stold(c.text.substr(0,comma)),std::stold(c.text.substr(comma+1)));
                break;
            }
            case Op::PiStep: core_.piStep(std::stold(c.text)); break;
            case Op::ConfigureNano:
                core_.configureNanoBytes(static_cast<u32>(c.arg[0]),static_cast<u32>(c.arg[1]),static_cast<u32>(c.arg[2])); break;
            case Op::NanoRead: {
                const auto length=static_cast<std::size_t>(c.arg[1]);
                r.bytes.resize(length);
                r.accepted=core_.nanoRead(static_cast<u64>(c.arg[0]),r.bytes.data(),length);
                if(!r.accepted) r.bytes.clear();
                break;
            }
        }
        return r;
    }

    Result invoke(Command c) {
        c.sequence=nextSequence_;
        const auto wire=MorsePacket::encode(c);
        const auto decoded=MorsePacket::decode(wire);
        if(!experience_.observe(decoded.sequence).valid ||
           !experience_.validate(decoded.sequence,decoded.sequence==nextSequence_).valid)
            throw std::runtime_error("VME Experience validation rejected command");
        BlackBox snapshot=core_;
        try {
            auto result=dispatch(decoded);
            if(!experience_.transform(decoded.sequence,true).valid ||
               !experience_.commit(decoded.sequence,true).valid)
                throw std::runtime_error("VME Experience commit rejected command");
            ++nextSequence_;
            return result;
        } catch(...) {
            core_=snapshot;
            throw;
        }
    }

public:
    void reset() { Command c; c.op=Op::Reset; (void)invoke(c); }
    void load(i64 x) { Command c; c.op=Op::Load; c.arg[0]=x; (void)invoke(c); }
    i64 next() { Command c; c.op=Op::Next; return invoke(c).value; }
    i64 same(i64 a,i64 b) { Command c; c.op=Op::Same;c.arg[0]=a;c.arg[1]=b;return invoke(c).value; }
    i64 opposite(i64 a,i64 b) { Command c;c.op=Op::Opposite;c.arg[0]=a;c.arg[1]=b;return invoke(c).value; }
    i64 reinforce(i64 a,i64 b) { Command c;c.op=Op::Reinforce;c.arg[0]=a;c.arg[1]=b;return invoke(c).value; }
    void setCZCoefficients(i64 a,i64 b,i64 g) { Command c;c.op=Op::SetCZ;c.arg[0]=a;c.arg[1]=b;c.arg[2]=g;(void)invoke(c); }
    i64 czProcess(i64 p,i64 m) { Command c;c.op=Op::CZProcess;c.arg[0]=p;c.arg[1]=m;return invoke(c).value; }
    void setEnergy(i64 p,i64 n) { Command c;c.op=Op::SetEnergy;c.arg[0]=p;c.arg[1]=n;(void)invoke(c); }
    i64 dpzZeroTimesZero() { Command c;c.op=Op::DPZZeroTimesZero;return invoke(c).value; }
    void dpzMultiplyZero(i64 x) { Command c;c.op=Op::DPZMultiplyZero;c.arg[0]=x;(void)invoke(c); }
    void dpzDivideZero(i64 x) { Command c;c.op=Op::DPZDivideZero;c.arg[0]=x;(void)invoke(c); }
    void dpzAddZero(i64 x) { Command c;c.op=Op::DPZAddZero;c.arg[0]=x;(void)invoke(c); }
    void dpzZeroMinus(i64 x) { Command c;c.op=Op::DPZZeroMinus;c.arg[0]=x;(void)invoke(c); }
    i64 dpzZeroPowerZero() { Command c;c.op=Op::DPZZeroPowerZero;return invoke(c).value; }
    void dpzMultiply(i64 a,i64 b) { Command c;c.op=Op::DPZMultiply;c.arg[0]=a;c.arg[1]=b;(void)invoke(c); }
    void dpzDivide(i64 a,i64 b) { Command c;c.op=Op::DPZDivide;c.arg[0]=a;c.arg[1]=b;(void)invoke(c); }
    void dpzAdd(i64 a,i64 b) { Command c;c.op=Op::DPZAdd;c.arg[0]=a;c.arg[1]=b;(void)invoke(c); }
    void dpzSubtract(i64 a,i64 b) { Command c;c.op=Op::DPZSubtract;c.arg[0]=a;c.arg[1]=b;(void)invoke(c); }
    void configurePi(long double omega,long double radialScale) { Command c;c.op=Op::ConfigurePi;c.text=realText(omega)+","+realText(radialScale);(void)invoke(c); }
    void piStep(long double dt) { Command c;c.op=Op::PiStep;c.text=realText(dt);(void)invoke(c); }
    void configureNanoBytes(u32 seed,u32 scale,u32 offset) { Command c;c.op=Op::ConfigureNano;c.arg[0]=seed;c.arg[1]=scale;c.arg[2]=offset;(void)invoke(c); }
    bool nanoRead(u64 offset,u8* output,std::size_t length) {
        if(output==nullptr || length==0 || length>65536) return false;
        Command c;c.op=Op::NanoRead;c.arg[0]=static_cast<i64>(offset);c.arg[1]=static_cast<i64>(length);
        auto r=invoke(c); if(!r.accepted) return false;
        std::copy(r.bytes.begin(),r.bytes.end(),output); return true;
    }

    i64 viorBinaryBit(int bit) {
        return core_.applyBinaryBehavior(bit);
    }

    const State& inspect() const { return core_.inspect(); }
    u32 token() const { return core_.token(); }
};

class ViorBinary {
public:
    struct Summary {
        u64 totalBytes = 0;
        u64 zeroBits = 0;
        u64 oneBits = 0;
        i64 finalState = 0;
        u32 finalToken = 0;
    };

    static void feedChunkBits(Runtime& rt, const u8* data, std::size_t len, Summary& ioSummary) {
        for (std::size_t i = 0; i < len; ++i) {
            const u8 byte = data[i];
            ++ioSummary.totalBytes;

            for (int bitPos = 7; bitPos >= 0; --bitPos) {
                const int bit = (byte >> bitPos) & 1;
                if (bit == 0) ++ioSummary.zeroBits;
                else ++ioSummary.oneBits;
                rt.viorBinaryBit(bit);
            }

            const auto& st = rt.inspect();
            const u32 nextSeed = 0x13570000u ^ (st.token << 4) ^ static_cast<u32>(byte);
            rt.configureNanoBytes(nextSeed, st.nanoScaleQ16, st.nanoOffsetQ16);
        }
        const auto& finalSt = rt.inspect();
        ioSummary.finalState = finalSt.boundedState;
        ioSummary.finalToken = finalSt.token;
    }

    static bool runFile(const std::string& inputPath, Summary& outSummary) {
        std::ifstream fin(inputPath, std::ios::binary);
        if (!fin) {
            std::cerr << "Failed to open input file for ViorBinary: " << inputPath << "\n";
            return false;
        }

        Runtime rt;
        rt.reset();
        rt.setCZCoefficients(1, 1, 0);
        rt.configurePi(1.0L, 1.0L);

        outSummary = Summary{};
        std::vector<u8> buf(65536);

        while (fin) {
            fin.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
            const auto got = static_cast<std::size_t>(fin.gcount());
            if (got == 0) break;
            feedChunkBits(rt, buf.data(), got, outSummary);
        }

        const auto& finalSt = rt.inspect();
        std::cout << "=== ViorBinary Stream Result ===\n"
                  << "Input File   : " << inputPath << "\n"
                  << "Bytes Read   : " << outSummary.totalBytes << "\n"
                  << "Zero Bits (0): " << outSummary.zeroBits << "\n"
                  << "One Bits  (1): " << outSummary.oneBits << "\n"
                  << "Final State  : " << outSummary.finalState << "\n"
                  << "Behavior     : [" << finalSt.behavior[0] << "," << finalSt.behavior[1] << ","
                  << finalSt.behavior[2] << "," << finalSt.behavior[3] << "]\n"
                  << "Equilibrium  : " << (finalSt.equilibrium ? "true" : "false") << "\n"
                  << "Final Token  : 0x" << std::hex << std::setw(8) << std::setfill('0')
                  << outSummary.finalToken << std::dec << "\n";
        return true;
    }
};

class CZFilePipeline {
private:
    static constexpr u32 FNV_OFFSET = 2166136261u;
    static constexpr u32 FNV_PRIME  = 16777619u;
    static constexpr std::size_t DEFAULT_CHUNK = 65536;

    static u32 fnvUpdate(u32 h, const u8* data, std::size_t len) {
        for (std::size_t i = 0; i < len; ++i) {
            h ^= data[i];
            h *= FNV_PRIME;
        }
        return h;
    }

    static bool writeU32(std::ostream& out, u32 v) {
        u8 b[4]{
            static_cast<u8>(v & 0xffu),
            static_cast<u8>((v >> 8) & 0xffu),
            static_cast<u8>((v >> 16) & 0xffu),
            static_cast<u8>((v >> 24) & 0xffu)
        };
        return static_cast<bool>(out.write(reinterpret_cast<const char*>(b), 4));
    }

    static bool writeU64(std::ostream& out, u64 v) {
        for (unsigned i = 0; i < 8; ++i) {
            const u8 byte = static_cast<u8>((v >> (8u * i)) & 0xffu);
            if (!out.write(reinterpret_cast<const char*>(&byte), 1)) return false;
        }
        return true;
    }

    static bool readU32(std::istream& in, u32& v) {
        u8 b[4]{};
        if (!in.read(reinterpret_cast<char*>(b), 4)) return false;
        v = static_cast<u32>(b[0]) |
            (static_cast<u32>(b[1]) << 8) |
            (static_cast<u32>(b[2]) << 16) |
            (static_cast<u32>(b[3]) << 24);
        return true;
    }

    static bool readU64(std::istream& in, u64& v) {
        u8 b[8]{};
        if (!in.read(reinterpret_cast<char*>(b), 8)) return false;
        v = 0;
        for (unsigned i = 0; i < 8; ++i) {
            v |= static_cast<u64>(b[i]) << (8u * i);
        }
        return true;
    }

    static std::vector<u8> stepNanoMask(Runtime& rt, u64 chunkIdx, std::size_t len) {
        const i64 stateVal = rt.czProcess(static_cast<i64>(chunkIdx + 1), 1);
        rt.piStep(0.01L);
        const u32 tok = rt.token();
        const u32 seed = rt.inspect().nanoSeedQ16 ^ (tok << 8) ^ static_cast<u32>(stateVal);
        const u32 scale = rt.inspect().nanoScaleQ16 ^ (static_cast<u32>(chunkIdx) << 12);
        const u32 offset = rt.inspect().nanoOffsetQ16 ^ (tok >> 4);
        rt.configureNanoBytes(seed, scale, offset);

        std::vector<u8> mask(len);
        if (len == 0) return mask;

        constexpr u64 cap = 2048ULL * 512ULL;
        const u64 span = cap - static_cast<u64>(len);
        const u64 virtOffset = (span > 0) ? ((chunkIdx * 4096ULL) % span) : 0ULL;
        if (!rt.nanoRead(virtOffset, mask.data(), len)) {
            throw std::runtime_error("NanoRead failed during CZ chunk transform");
        }
        return mask;
    }

    static std::pair<u8, std::vector<u8>> encodeMaskedChunk(const std::vector<u8>& masked) {
        std::vector<u8> rle;
        rle.reserve(masked.size());
        std::size_t i = 0;
        while (i < masked.size()) {
            const u8 val = masked[i];
            std::size_t run = 1;
            while (i + run < masked.size() && masked[i + run] == val && run < 255) {
                ++run;
            }
            rle.push_back(static_cast<u8>(run));
            rle.push_back(val);
            i += run;
        }
        if (rle.size() < masked.size()) {
            return {1u, std::move(rle)};
        }
        return {0u, masked};
    }

    static std::vector<u8> decodeMaskedChunk(u8 mode, const std::vector<u8>& payload,
                                             std::size_t expectedLen) {
        if (mode == 0u) {
            if (payload.size() != expectedLen) {
                throw std::runtime_error("Raw chunk size mismatch");
            }
            return payload;
        }
        if (mode == 1u) {
            std::vector<u8> out;
            out.reserve(expectedLen);
            for (std::size_t i = 0; i + 1 < payload.size(); i += 2) {
                const std::size_t run = payload[i];
                const u8 val = payload[i + 1];
                if (out.size() + run > expectedLen) {
                    throw std::runtime_error("RLE decoded length overflow");
                }
                out.insert(out.end(), run, val);
            }
            if (out.size() != expectedLen) {
                throw std::runtime_error("RLE decoded length mismatch");
            }
            return out;
        }
        throw std::runtime_error("Unknown CZ chunk mode");
    }

public:
    static bool packFile(const std::string& inputPath, const std::string& outputCzPath) {
        std::ifstream fin(inputPath, std::ios::binary | std::ios::ate);
        if (!fin) {
            std::cerr << "Failed to open input file: " << inputPath << "\n";
            return false;
        }
        const u64 origSize = static_cast<u64>(fin.tellg());
        fin.seekg(0, std::ios::beg);

        Runtime sigRt;
        sigRt.reset();
        sigRt.setCZCoefficients(1, 1, 0);
        sigRt.configurePi(1.0L, 1.0L);

        u32 origHash = FNV_OFFSET;
        ViorBinary::Summary viorSummary{};
        std::vector<u8> scanBuf(DEFAULT_CHUNK);
        while (fin) {
            fin.read(reinterpret_cast<char*>(scanBuf.data()), static_cast<std::streamsize>(scanBuf.size()));
            const auto got = static_cast<std::size_t>(fin.gcount());
            if (got > 0) {
                origHash = fnvUpdate(origHash, scanBuf.data(), got);
                ViorBinary::feedChunkBits(sigRt, scanBuf.data(), got, viorSummary);
            }
        }
        fin.clear();
        fin.seekg(0, std::ios::beg);

        std::ofstream fout(outputCzPath, std::ios::binary | std::ios::trunc);
        if (!fout) {
            std::cerr << "Failed to open output .cz file: " << outputCzPath << "\n";
            return false;
        }

        Runtime rt;
        rt.reset();
        rt.setCZCoefficients(1, 1, 0);
        rt.configurePi(1.0L, 1.0L);

        const auto& st = rt.inspect();
        if (!fout.write("CZF2", 4) ||
            !writeU64(fout, origSize) ||
            !writeU64(fout, static_cast<u64>(DEFAULT_CHUNK)) ||
            !writeU32(fout, st.nanoSeedQ16) ||
            !writeU32(fout, st.nanoScaleQ16) ||
            !writeU32(fout, st.nanoOffsetQ16) ||
            !writeU32(fout, origHash) ||
            !writeU64(fout, viorSummary.zeroBits) ||
            !writeU64(fout, viorSummary.oneBits) ||
            !writeU32(fout, viorSummary.finalToken)) {
            std::cerr << "Failed writing .cz header\n";
            return false;
        }

        u64 chunkIdx = 0;
        ViorBinary::Summary packBits{};
        std::vector<u8> inBuf(DEFAULT_CHUNK);
        while (fin) {
            fin.read(reinterpret_cast<char*>(inBuf.data()), static_cast<std::streamsize>(inBuf.size()));
            const auto got = static_cast<std::size_t>(fin.gcount());
            if (got == 0) break;

            ViorBinary::feedChunkBits(rt, inBuf.data(), got, packBits);
            const std::vector<u8> mask = stepNanoMask(rt, chunkIdx, got);
            std::vector<u8> masked(got);
            for (std::size_t i = 0; i < got; ++i) {
                masked[i] = static_cast<u8>(inBuf[i] ^ mask[i]);
            }

            auto [mode, payload] = encodeMaskedChunk(masked);
            const char modeChar = static_cast<char>(mode);
            if (!writeU32(fout, static_cast<u32>(payload.size())) ||
                !fout.write(&modeChar, 1)) {
                std::cerr << "Failed writing chunk metadata\n";
                return false;
            }
            if (!payload.empty() &&
                !fout.write(reinterpret_cast<const char*>(payload.data()),
                            static_cast<std::streamsize>(payload.size()))) {
                std::cerr << "Failed writing chunk payload\n";
                return false;
            }
            ++chunkIdx;
        }

        fout.flush();
        if (!fout) {
            std::cerr << "Error finalizing .cz output stream\n";
            return false;
        }
        const u64 czSize = static_cast<u64>(fout.tellp());
        fout.close();

        std::cout << "Packed '" << inputPath << "' -> '" << outputCzPath << "'\n"
                  << "Original Bytes : " << origSize << "\n"
                  << "CZ Bytes       : " << czSize << "\n"
                  << "Chunks         : " << chunkIdx << "\n"
                  << "Vior 0/1 Bits  : " << viorSummary.zeroBits << " / " << viorSummary.oneBits << "\n"
                  << "Vior Token     : 0x" << std::hex << std::setw(8) << std::setfill('0')
                  << viorSummary.finalToken << std::dec << "\n"
                  << "FNV-1a Hash    : 0x" << std::hex << origHash << std::dec << "\n";
        return true;
    }

    static bool unpackFile(const std::string& inputCzPath, const std::string& restoredPath) {
        std::ifstream fin(inputCzPath, std::ios::binary);
        if (!fin) {
            std::cerr << "Failed to open .cz file: " << inputCzPath << "\n";
            return false;
        }

        char magic[4]{};
        u64 origSize = 0, chunkSize64 = 0, hdrZeroBits = 0, hdrOneBits = 0;
        u32 seed = 0, scale = 0, offset = 0, origHash = 0, hdrViorToken = 0;

        if (!fin.read(magic, 4) || std::string(magic, 4) != "CZF2" ||
            !readU64(fin, origSize) || !readU64(fin, chunkSize64) ||
            !readU32(fin, seed) || !readU32(fin, scale) ||
            !readU32(fin, offset) || !readU32(fin, origHash) ||
            !readU64(fin, hdrZeroBits) || !readU64(fin, hdrOneBits) ||
            !readU32(fin, hdrViorToken)) {
            std::cerr << "Invalid or truncated .cz header\n";
            return false;
        }

        std::ofstream fout(restoredPath, std::ios::binary | std::ios::trunc);
        if (!fout) {
            std::cerr << "Failed to open restored output file: " << restoredPath << "\n";
            return false;
        }

        Runtime rt;
        rt.reset();
        rt.setCZCoefficients(1, 1, 0);
        rt.configurePi(1.0L, 1.0L);

        u64 writtenTotal = 0;
        u64 chunkIdx = 0;
        u32 recLen = 0;
        u32 restoredHash = FNV_OFFSET;
        ViorBinary::Summary restoredVior{};

        while (readU32(fin, recLen)) {
            char modeChar = 0;
            if (!fin.read(&modeChar, 1)) {
                std::cerr << "Truncated chunk mode in .cz file\n";
                return false;
            }
            std::vector<u8> payload(recLen);
            if (recLen > 0 && !fin.read(reinterpret_cast<char*>(payload.data()), recLen)) {
                std::cerr << "Truncated chunk payload in .cz file\n";
                return false;
            }
            if (writtenTotal >= origSize) {
                std::cerr << "Unexpected trailing chunk beyond original size\n";
                return false;
            }

            const std::size_t expectedChunkLen = static_cast<std::size_t>(
                std::min<u64>(chunkSize64, origSize - writtenTotal));
            ViorBinary::Summary preChunkBits = restoredVior;
            const std::vector<u8> mask = stepNanoMask(rt, chunkIdx, expectedChunkLen);
            restoredVior = preChunkBits;

            const std::vector<u8> masked = decodeMaskedChunk(static_cast<u8>(modeChar), payload, expectedChunkLen);

            std::vector<u8> origChunk(expectedChunkLen);
            for (std::size_t i = 0; i < expectedChunkLen; ++i) {
                origChunk[i] = static_cast<u8>(masked[i] ^ mask[i]);
            }

            ViorBinary::feedChunkBits(rt, origChunk.data(), origChunk.size(), restoredVior);
            if (!origChunk.empty() &&
                !fout.write(reinterpret_cast<const char*>(origChunk.data()),
                            static_cast<std::streamsize>(origChunk.size()))) {
                std::cerr << "Failed writing restored output\n";
                return false;
            }
            restoredHash = fnvUpdate(restoredHash, origChunk.data(), origChunk.size());
            writtenTotal += origChunk.size();
            ++chunkIdx;
        }

        fout.flush();
        const bool sizeOk = (writtenTotal == origSize);
        const bool hashOk = (restoredHash == origHash);
        const bool viorOk = (restoredVior.zeroBits == hdrZeroBits) &&
                            (restoredVior.oneBits == hdrOneBits) &&
                            (restoredVior.finalToken == hdrViorToken);

        std::cout << "Restored '" << inputCzPath << "' -> '" << restoredPath << "'\n"
                  << "Restored Bytes : " << writtenTotal << " / " << origSize << "\n"
                  << "Vior 0/1 Bits  : " << restoredVior.zeroBits << " / " << restoredVior.oneBits << "\n"
                  << "Vior Token     : 0x" << std::hex << std::setw(8) << std::setfill('0')
                  << restoredVior.finalToken << std::dec << "\n"
                  << "FNV-1a Hash    : 0x" << std::hex << restoredHash << std::dec << "\n"
                  << "Integrity Match: " << (sizeOk && hashOk && viorOk ? "PASS" : "FAIL") << "\n";
        return sizeOk && hashOk && viorOk;
    }
};

template<class EngineType>
static void printState(const EngineType& engine, const std::string& label) {
    const State& s = engine.inspect();
    std::cout << "\n=== " << label << " ===\n"
              << "UBX state: " << s.boundedState << "\n"
              << "Behavior: [" << s.behavior[0] << "," << s.behavior[1] << ","
              << s.behavior[2] << "," << s.behavior[3] << "]\n"
              << "CZ plus/minus: " << s.plusVector << " / " << s.minusVector << "\n"
              << "CZ equilibrium error: " << s.equilibriumError
              << " | equilibrium: " << (s.equilibrium ? "true" : "false") << "\n"
              << "Energy balance: " << (s.balanced ? "true" : "false") << "\n"
              << "DPZ type: " << static_cast<int>(s.dpz.type)
              << " | latent Px: " << s.dpz.latentMemory
              << " | field source Phi_x: " << s.dpz.fieldSource << "\n"
              << std::setprecision(18)
              << "Pi k: " << s.pi.k
              << " | theta: " << s.pi.theta
              << " | omega: " << s.pi.omega
              << " | deltaR: " << s.pi.deltaR << "\n"
              << "Nano-Bytes reads: " << s.nanoReadCount
              << " | last offset: " << s.nanoLastIndex
              << " | checksum: 0x" << std::hex << s.nanoLastChecksum << std::dec << "\n"
              << "Nano matrix Q16.16: seed=0x" << std::hex << s.nanoSeedQ16
              << " scale=0x" << s.nanoScaleQ16 << " offset=0x" << s.nanoOffsetQ16
              << std::dec << "\n"
              << "Opaque token (not encryption): 0x"
              << std::hex << std::setw(8) << std::setfill('0') << s.token
              << std::dec << "\n";
}

} // namespace vcore

int main(int argc, char** argv) {
    using namespace vcore;
    vme::Runtime engine;

    if (argc == 3 && std::string(argv[1]) == "--vior-binary") {
        vme::ViorBinary::Summary summary{};
        return vme::ViorBinary::runFile(argv[2], summary) ? 0 : 1;
    }

    if (argc == 4 && std::string(argv[1]) == "--pack") {
        return vme::CZFilePipeline::packFile(argv[2], argv[3]) ? 0 : 1;
    }

    if (argc == 4 && std::string(argv[1]) == "--unpack") {
        return vme::CZFilePipeline::unpackFile(argv[2], argv[3]) ? 0 : 1;
    }

    if (argc == 4 && std::string(argv[1]) == "--nano-read") {
        try {
            const auto offset = std::stoull(argv[2]);
            const auto length64 = std::stoull(argv[3]);
            if (length64 == 0 || length64 > 65536) {
                std::cerr << "Length must be in range 1..65536 bytes.\n";
                return 2;
            }
            std::vector<u8> bytes(static_cast<std::size_t>(length64));
            if (!engine.nanoRead(offset, bytes.data(), bytes.size())) {
                std::cerr << "Read outside virtual capacity (1 MiB).\n";
                return 2;
            }
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                if (i && i % 16 == 0) std::cout << "\n";
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned>(bytes[i]) << " ";
            }
            std::cout << std::dec << "\n";
            printState(engine, "Nano-Bytes read status");
            return 0;
        } catch (...) {
            std::cerr << "Invalid offset/length.\n";
            return 2;
        }
    }

    if (argc != 1) {
        std::cerr << "Usage:\n"
                  << "  " << argv[0] << "\n"
                  << "  " << argv[0] << " --vior-binary INPUT_FILE\n"
                  << "  " << argv[0] << " --nano-read OFFSET LENGTH\n"
                  << "  " << argv[0] << " --pack INPUT_FILE OUTPUT.cz\n"
                  << "  " << argv[0] << " --unpack INPUT.cz RESTORED_FILE\n";
        return 2;
    }

    engine.load(7);
    printState(engine, "UBX load");

    engine.load(9);
    engine.next();
    printState(engine, "UBX boundary 9 -> 0");

    engine.reset();
    engine.setCZCoefficients(1, 1, 0);
    engine.czProcess(50, 30);
    printState(engine, "CZ differential +50 - 30");

    engine.reset();
    engine.dpzZeroTimesZero();
    printState(engine, "DPZ 0 x 0 -> Origin");

    engine.reset();
    engine.dpzMultiplyZero(7);
    printState(engine, "DPZ 7 x 0 -> P7 latent");

    engine.reset();
    engine.dpzDivideZero(7);
    printState(engine, "DPZ 7 / 0 -> Phi7 field");

    engine.reset();
    engine.dpzAddZero(7);
    printState(engine, "DPZ 7 + 0 -> 7* primed");

    engine.reset();
    engine.dpzZeroMinus(7);
    printState(engine, "DPZ 0 - 7 -> absorbed inverse");

    engine.reset();
    engine.dpzZeroPowerZero();
    printState(engine, "DPZ 0^0 -> Origin");

    engine.reset();
    engine.configurePi(1.0L, 1.0L);
    for (int i = 0; i < 3; ++i) engine.piStep(0.01L);
    printState(engine, "Pi behavioral state after 3 steps");

    std::cout << "\nUBX + CZ + DPZ + Pi reference engine complete.\n";
    return 0;
}
