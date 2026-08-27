#include "../src/services/S3Response.h"
#include <cassert>
#include <iostream>
/**
 * PostgreSQL hands back "2026-08-27 19:46:58.845507+00"; every S3 SDK expects
 * ISO8601 and fails to parse anything else into a date. These are the shapes
 * the column actually produces.
 */
int main() {
  using s3::isoTimestamp;
  // What PostgreSQL actually hands back, and what S3 clients require.
  assert(isoTimestamp("2026-08-27 19:46:58.845507+00") == "2026-08-27T19:46:58.845Z");
  // Whole seconds, no fractional part.
  assert(isoTimestamp("2026-08-27 19:46:58+00") == "2026-08-27T19:46:58Z");
  // Fewer than three fractional digits must still pad to milliseconds.
  assert(isoTimestamp("2026-08-27 19:46:58.5+00") == "2026-08-27T19:46:58.500Z");
  // Already ISO, or unrecognised: passed through rather than mangled.
  assert(isoTimestamp("2026-08-27T19:46:58Z") == "2026-08-27T19:46:58Z");
  assert(isoTimestamp("") == "");
  std::cout << "isoTimestamp: all cases pass\n";
}
