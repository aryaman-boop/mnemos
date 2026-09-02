// grisu2, ported from fpconv (github.com/night-shift/fpconv) -- the same file
// Redis vendors, so the digits come out identical by construction rather than
// by agreement between two shortest-representation algorithms. Grisu2 is not
// always optimal: on a small fraction of doubles it emits one digit more than
// the true shortest form, and reproducing *that* is the point.
#include "core/dtoa.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace mnemos::core {
namespace {

constexpr std::uint64_t kFracMask  = 0x000FFFFFFFFFFFFFULL;
constexpr std::uint64_t kExpMask   = 0x7FF0000000000000ULL;
constexpr std::uint64_t kHiddenBit = 0x0010000000000000ULL;
constexpr std::uint64_t kSignMask  = 0x8000000000000000ULL;
constexpr int           kExpBias   = 1075;

// A binary floating-point value as frac * 2^exp, with more bits of headroom
// than a double has -- grisu2 works by keeping the error in those spare bits.
struct Fp {
    std::uint64_t frac;
    int           exp;
};

constexpr std::uint64_t kTens[] = {
    10000000000000000000ULL, 1000000000000000000ULL, 100000000000000000ULL,
    10000000000000000ULL,    1000000000000000ULL,    100000000000000ULL,
    10000000000000ULL,       1000000000000ULL,       100000000000ULL,
    10000000000ULL,          1000000000ULL,          100000000ULL,
    10000000ULL,             1000000ULL,             100000ULL,
    10000ULL,                1000ULL,                100ULL,
    10ULL,                   1ULL};

// Powers of ten from 10^-348 to 10^340 in steps of eight, each normalised so
// the significand fills all 64 bits. Eight is close enough that one
// multiplication by the nearest entry brings any double into the range the
// digit generator needs.
constexpr int kFirstPower = -348;
constexpr int kStepPowers = 8;
constexpr int kNumPowers  = 87;
constexpr int kExpMin     = -60;
constexpr int kExpMax     = -32;

constexpr Fp kPowersTen[kNumPowers] = {
    {18054884314459144840ULL, -1220}, {13451937075301367670ULL, -1193}, {10022474136428063862ULL, -1166},
    {14934650266808366570ULL, -1140}, {11127181549972568877ULL, -1113}, {16580792590934885855ULL, -1087},
    {12353653155963782858ULL, -1060}, {18408377700990114895ULL, -1034}, {13715310171984221708ULL, -1007},
    {10218702384817765436ULL, -980}, {15227053142812498563ULL, -954}, {11345038669416679861ULL, -927},
    {16905424996341287883ULL, -901}, {12595523146049147757ULL, -874}, {9384396036005875287ULL, -847},
    {13983839803942852151ULL, -821}, {10418772551374772303ULL, -794}, {15525180923007089351ULL, -768},
    {11567161174868858868ULL, -741}, {17236413322193710309ULL, -715}, {12842128665889583758ULL, -688},
    {9568131466127621947ULL, -661}, {14257626930069360058ULL, -635}, {10622759856335341974ULL, -608},
    {15829145694278690180ULL, -582}, {11793632577567316726ULL, -555}, {17573882009934360870ULL, -529},
    {13093562431584567480ULL, -502}, {9755464219737475723ULL, -475}, {14536774485912137811ULL, -449},
    {10830740992659433045ULL, -422}, {16139061738043178685ULL, -396}, {12024538023802026127ULL, -369},
    {17917957937422433684ULL, -343}, {13349918974505688015ULL, -316}, {9946464728195732843ULL, -289},
    {14821387422376473014ULL, -263}, {11042794154864902060ULL, -236}, {16455045573212060422ULL, -210},
    {12259964326927110867ULL, -183}, {18268770466636286478ULL, -157}, {13611294676837538539ULL, -130},
    {10141204801825835212ULL, -103}, {15111572745182864684ULL, -77}, {11258999068426240000ULL, -50},
    {16777216000000000000ULL, -24}, {12500000000000000000ULL, 3}, {9313225746154785156ULL, 30},
    {13877787807814456755ULL, 56}, {10339757656912845936ULL, 83}, {15407439555097886824ULL, 109},
    {11479437019748901445ULL, 136}, {17105694144590052135ULL, 162}, {12744735289059618216ULL, 189},
    {9495567745759798747ULL, 216}, {14149498560666738074ULL, 242}, {10542197943230523224ULL, 269},
    {15709099088952724970ULL, 295}, {11704190886730495818ULL, 322}, {17440603504673385349ULL, 348},
    {12994262207056124023ULL, 375}, {9681479787123295682ULL, 402}, {14426529090290212157ULL, 428},
    {10748601772107342003ULL, 455}, {16016664761464807395ULL, 481}, {11933345169920330789ULL, 508},
    {17782069995880619868ULL, 534}, {13248674568444952270ULL, 561}, {9871031767461413346ULL, 588},
    {14708983551653345445ULL, 614}, {10959046745042015199ULL, 641}, {16330252207878254650ULL, 667},
    {12166986024289022870ULL, 694}, {18130221999122236476ULL, 720}, {13508068024458167312ULL, 747},
    {10064294952495520794ULL, 774}, {14996968138956309548ULL, 800}, {11173611982879273257ULL, 827},
    {16649979327439178909ULL, 853}, {12405201291620119593ULL, 880}, {9242595204427927429ULL, 907},
    {13772540099066387757ULL, 933}, {10261342003245940623ULL, 960}, {15290591125556738113ULL, 986},
    {11392378155556871081ULL, 1013}, {16975966327722178521ULL, 1039}, {12648080533535911531ULL, 1066}
};

Fp findCachedPow10(int exp, int* k) {
    constexpr double kOneOverLogTen = 0.30102999566398114;

    const int approx = static_cast<int>(-(exp + kNumPowers) * kOneOverLogTen);
    int       idx    = (approx - kFirstPower) / kStepPowers;

    while (true) {
        const int current = exp + kPowersTen[idx].exp + 64;
        if (current < kExpMin) { ++idx; continue; }
        if (current > kExpMax) { --idx; continue; }
        *k = kFirstPower + idx * kStepPowers;
        return kPowersTen[idx];
    }
}

std::uint64_t bitsOf(double d) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
}

Fp buildFp(double d) {
    const std::uint64_t bits = bitsOf(d);

    Fp fp;
    fp.frac = bits & kFracMask;
    fp.exp  = static_cast<int>((bits & kExpMask) >> 52);

    if (fp.exp != 0) {
        fp.frac += kHiddenBit;
        fp.exp  -= kExpBias;
    } else {
        fp.exp = -kExpBias + 1;  // subnormal: no hidden bit, fixed exponent
    }
    return fp;
}

void normalize(Fp& fp) {
    while ((fp.frac & kHiddenBit) == 0) {
        fp.frac <<= 1;
        --fp.exp;
    }
    constexpr int kShift = 64 - 52 - 1;
    fp.frac <<= kShift;
    fp.exp   -= kShift;
}

// The half-way points to the neighbouring doubles. Any digit string that lands
// strictly between them reads back as `fp`, which is what makes a short one
// safe to emit.
void normalizedBoundaries(const Fp& fp, Fp& lower, Fp& upper) {
    upper.frac = (fp.frac << 1) + 1;
    upper.exp  = fp.exp - 1;
    while ((upper.frac & (kHiddenBit << 1)) == 0) {
        upper.frac <<= 1;
        --upper.exp;
    }
    constexpr int kUShift = 64 - 54;
    upper.frac <<= kUShift;
    upper.exp   -= kUShift;

    // A power of two has a closer neighbour below it than above.
    const int l_shift = fp.frac == kHiddenBit ? 2 : 1;
    lower.frac = (fp.frac << l_shift) - 1;
    lower.exp  = fp.exp - l_shift;

    lower.frac <<= lower.exp - upper.exp;
    lower.exp    = upper.exp;
}

Fp multiply(const Fp& a, const Fp& b) {
    constexpr std::uint64_t kLoMask = 0x00000000FFFFFFFFULL;

    const std::uint64_t ah_bl = (a.frac >> 32) * (b.frac & kLoMask);
    const std::uint64_t al_bh = (a.frac & kLoMask) * (b.frac >> 32);
    const std::uint64_t al_bl = (a.frac & kLoMask) * (b.frac & kLoMask);
    const std::uint64_t ah_bh = (a.frac >> 32) * (b.frac >> 32);

    std::uint64_t tmp = (ah_bl & kLoMask) + (al_bh & kLoMask) + (al_bl >> 32);
    tmp += 1ULL << 31;  // round the discarded low half

    return Fp{ah_bh + (ah_bl >> 32) + (al_bh >> 32) + (tmp >> 32), a.exp + b.exp + 64};
}

// Walks the last digit back while a smaller one is still closer to the true
// value -- the step that turns "a string that round-trips" into "the string
// nearest the double".
void roundDigit(char* digits, int ndigits, std::uint64_t delta, std::uint64_t rem,
                std::uint64_t kappa, std::uint64_t frac) {
    while (rem < frac && delta - rem >= kappa &&
           (rem + kappa < frac || frac - rem > rem + kappa - frac)) {
        --digits[ndigits - 1];
        rem += kappa;
    }
}

int generateDigits(const Fp& fp, const Fp& upper, const Fp& lower, char* digits, int* K) {
    const std::uint64_t wfrac = upper.frac - fp.frac;
    std::uint64_t       delta = upper.frac - lower.frac;

    const Fp one{1ULL << -upper.exp, upper.exp};

    std::uint64_t part1 = upper.frac >> -one.exp;
    std::uint64_t part2 = upper.frac & (one.frac - 1);

    int idx = 0;
    int kappa = 10;

    // The integral part, digit by digit, largest divisor first.
    for (const std::uint64_t* divp = kTens + 10; kappa > 0; ++divp) {
        const std::uint64_t div   = *divp;
        const unsigned      digit = static_cast<unsigned>(part1 / div);

        if (digit != 0 || idx != 0) digits[idx++] = static_cast<char>(digit + '0');

        part1 -= digit * div;
        --kappa;

        const std::uint64_t tmp = (part1 << -one.exp) + part2;
        if (tmp <= delta) {  // inside the boundaries: stop, the rest is noise
            *K += kappa;
            roundDigit(digits, idx, delta, tmp, div << -one.exp, wfrac);
            return idx;
        }
    }

    // The fractional part, multiplying out one digit at a time.
    const std::uint64_t* unit = kTens + 18;
    while (true) {
        part2 *= 10;
        delta *= 10;
        --kappa;

        const unsigned digit = static_cast<unsigned>(part2 >> -one.exp);
        if (digit != 0 || idx != 0) digits[idx++] = static_cast<char>(digit + '0');

        part2 &= one.frac - 1;
        if (part2 < delta) {
            *K += kappa;
            roundDigit(digits, idx, delta, part2, one.frac, wfrac * *unit);
            return idx;
        }
        --unit;
    }
}

int grisu2(double d, char* digits, int* K) {
    Fp w = buildFp(d);

    Fp lower, upper;
    normalizedBoundaries(w, lower, upper);
    normalize(w);

    int      k  = 0;
    const Fp cp = findCachedPow10(upper.exp, &k);

    w     = multiply(w, cp);
    upper = multiply(upper, cp);
    lower = multiply(lower, cp);

    // Shrink the interval by one ulp on each side to absorb the rounding error
    // the multiplication just introduced.
    ++lower.frac;
    --upper.frac;

    *K = -k;
    return generateDigits(w, upper, lower, digits, K);
}

// fpconv's notation rule, and the reason `1e-6` prints as 0.000001 while
// `1e-7` prints as 1e-7.
int emitDigits(const char* digits, int ndigits, char* dest, int K, bool neg) {
    int exp = K + ndigits - 1;
    if (exp < 0) exp = -exp;

    if (K >= 0 && exp < ndigits + 7) {  // a plain integer, zero-padded
        std::memcpy(dest, digits, static_cast<std::size_t>(ndigits));
        std::memset(dest + ndigits, '0', static_cast<std::size_t>(K));
        return ndigits + K;
    }

    if (K < 0 && (K > -7 || exp < 4)) {  // a plain decimal
        const int offset = ndigits - (K < 0 ? -K : K);
        if (offset <= 0) {  // below 1.0: "0." and leading zeros
            std::memmove(dest + 2 - offset, digits, static_cast<std::size_t>(ndigits));
            dest[0] = '0';
            dest[1] = '.';
            std::memset(dest + 2, '0', static_cast<std::size_t>(-offset));
            return ndigits + 2 - offset;
        }
        std::memmove(dest + offset + 1, digits + offset,
                     static_cast<std::size_t>(ndigits - offset));
        std::memcpy(dest, digits, static_cast<std::size_t>(offset));
        dest[offset] = '.';
        return ndigits + 1;
    }

    const int limit = 18 - (neg ? 1 : 0);
    if (ndigits > limit) ndigits = limit;

    int idx = 0;
    dest[idx++] = digits[0];
    if (ndigits > 1) {
        dest[idx++] = '.';
        std::memcpy(dest + idx, digits + 1, static_cast<std::size_t>(ndigits - 1));
        idx += ndigits - 1;
    }
    // Redis's fpconv signs the exponent both ways; the upstream one writes only
    // the minus, so "1e+100" here would be "1e100" there.
    dest[idx++] = 'e';
    dest[idx++] = K + ndigits - 1 < 0 ? '-' : '+';

    int cent = 0;
    if (exp > 99) {
        cent = exp / 100;
        dest[idx++] = static_cast<char>(cent + '0');
        exp -= cent * 100;
    }
    if (exp > 9) {
        const int dec = exp / 10;
        dest[idx++] = static_cast<char>(dec + '0');
        exp -= dec * 10;
    } else if (cent != 0) {
        dest[idx++] = '0';
    }
    dest[idx++] = static_cast<char>(exp % 10 + '0');
    return idx;
}

}  // namespace

std::size_t fpconvDtoa(double value, char dest[32]) {
    char digits[18];

    int  len = 0;
    bool neg = false;
    if (bitsOf(value) & kSignMask) {
        dest[len++] = '-';
        neg = true;
    }

    if (value == 0.0) {
        dest[len++] = '0';
        return static_cast<std::size_t>(len);
    }
    const std::uint64_t bits = bitsOf(value);
    if ((bits & kExpMask) == kExpMask) {
        const char* what = (bits & kFracMask) ? "nan" : "inf";
        std::memcpy(dest + len, what, 3);
        return static_cast<std::size_t>(len + 3);
    }

    int       K       = 0;
    const int ndigits = grisu2(value, digits, &K);
    len += emitDigits(digits, ndigits, dest + len, K, neg);
    return static_cast<std::size_t>(len);
}

namespace {

// Redis's double2ll: true when `value` is an integer that a signed 64-bit
// integer holds exactly. The bound is half of LLONG_MAX, not all of it, which
// is Redis being conservative about the rounding in the comparison itself --
// (double)(LLONG_MAX/2) is 2^62, and the range check is written against that.
bool double2ll(double value, long long& out) {
    if (value < -4611686018427387904.0 || value > 4611686018427387904.0) return false;
    const long long ll = static_cast<long long>(value);
    if (static_cast<double>(ll) != value) return false;
    out = ll;
    return true;
}

}  // namespace

std::string d2string(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value > 0 ? "inf" : "-inf";

    long long ll = 0;
    if (double2ll(value, ll)) return std::to_string(ll);

    char              buf[32];
    const std::size_t n = fpconvDtoa(value, buf);
    return std::string(buf, n);
}

}  // namespace mnemos::core
