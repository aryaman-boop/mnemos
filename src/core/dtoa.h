// Redis's double-to-string conversion, which is not printf's.
//
// `%.17g` always round-trips, but it is not what Redis prints: Redis uses
// fpconv's grisu2, which emits the *shortest* digit string that reads back as
// the same double and then picks between plain and scientific notation by its
// own rule. The difference is visible everywhere a score or a float leaves the
// server -- ZSCORE says `1e+100` where `%.17g` says `1.0000000000000000e+100`
// -- and it is baked into files, because a listpack-encoded sorted set stores
// each score as exactly these bytes.
#pragma once

#include <cstddef>
#include <string>

namespace mnemos::core {

// The digits only: no sign handling of zero, no "inf"/"nan" special cases, and
// no integer fast path. Callers want `d2string`.
std::size_t fpconvDtoa(double value, char dest[32]);

// Redis's d2string(): what a double looks like everywhere it is written down --
// the text of a score inside a listpack, and the body of a `ZSCORE` reply. A
// double that is exactly an integer and small enough to survive the trip prints
// as that integer in full, which is not what the shortest-digits rule would
// give: 98765432109876496 has a shorter representation that reads back the
// same, and Redis prints neither it nor scientific notation.
std::string d2string(double value);

}  // namespace mnemos::core
