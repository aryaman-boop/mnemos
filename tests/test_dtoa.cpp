// Double formatting. Every expectation here was read off a live redis-server,
// because the point of the exercise is not that the digits round-trip -- lots
// of spellings do -- but that they are spelled the way Redis spells them, in a
// listpack score and in a reply alike.
#include "core/dtoa.h"

#include <cmath>
#include <limits>
#include <string>

#include "net/resp.h"
#include "test_harness.h"

using namespace mnemos;
using core::d2string;

namespace {

void testSpecials() {
    CHECK_EQ(d2string(std::numeric_limits<double>::quiet_NaN()), std::string("nan"));
    CHECK_EQ(d2string(std::numeric_limits<double>::infinity()), std::string("inf"));
    CHECK_EQ(d2string(-std::numeric_limits<double>::infinity()), std::string("-inf"));
}

void testIntegerFastPath() {
    CHECK_EQ(d2string(0.0), std::string("0"));
    CHECK_EQ(d2string(1.0), std::string("1"));
    CHECK_EQ(d2string(-1.0), std::string("-1"));
    CHECK_EQ(d2string(3.0e10), std::string("30000000000"));
    CHECK_EQ(d2string(1e18), std::string("1000000000000000000"));
    // double2ll's bound is (double)(LLONG_MAX/2) == 2^62, inclusive. One step
    // past it and the shortest-digits path takes over, which is why 4.6e18
    // prints in full and 4.7e18 does not.
    CHECK_EQ(d2string(4611686018427387904.0), std::string("4611686018427387904"));
    CHECK_EQ(d2string(4.6e18), std::string("4600000000000000000"));
    CHECK_EQ(d2string(4.7e18), std::string("4.7e+18"));
    CHECK_EQ(d2string(1e19), std::string("1e+19"));
    // The fast path swallows the sign of a negative zero. That is Redis's
    // d2string, and it is what a listpack-encoded score really contains.
    CHECK_EQ(d2string(-0.0), std::string("0"));
}

void testShortestDigits() {
    CHECK_EQ(d2string(1.5), std::string("1.5"));
    CHECK_EQ(d2string(-2.25), std::string("-2.25"));
    CHECK_EQ(d2string(0.0001), std::string("0.0001"));
    CHECK_EQ(d2string(1e-6), std::string("0.000001"));
    CHECK_EQ(d2string(-1e-7), std::string("-1e-7"));
    // Too big for double2ll, and still not scientific: fpconv writes the
    // shortest digits out in full as long as no more than seven zeros follow
    // them. Here seventeen digits are followed by five.
    CHECK_EQ(d2string(1.2345e21), std::string("1234499999999999900000"));
    CHECK_EQ(d2string(1e100), std::string("1e+100"));
    CHECK_EQ(d2string(5e-324), std::string("5e-324"));
    CHECK_EQ(d2string(633869777.43339539), std::string("6.338697774333954e+8"));
}

void testRoundTrip() {
    // Whatever the spelling, it has to read back as the same double -- a
    // RESTORE of a sorted set parses these very bytes.
    const double values[] = {0.5,   1.0 / 3.0, 1e-300, 1e300, 12345.6789, -98765.4321,
                             1e-5,  9.87e17,   2.5e18, 1.7976931348623157e308};
    for (double v : values) {
        CHECK(std::stod(d2string(v)) == v);
    }
}

void testReplyForm() {
    // A reply is d2string with exactly one case taken back.
    CHECK_EQ(net::formatDouble(-0.0), std::string("-0"));
    CHECK_EQ(net::formatDouble(0.0), std::string("0"));
    CHECK_EQ(net::formatDouble(1.5), std::string("1.5"));
    CHECK_EQ(net::formatDouble(std::numeric_limits<double>::infinity()), std::string("inf"));
}

}  // namespace

int main() {
    testSpecials();
    testIntegerFastPath();
    testShortestDigits();
    testRoundTrip();
    testReplyForm();
    return mnemos::test::summarise("dtoa");
}
