// test_wavdump.cpp — WavDump writes a valid WAV header and the recorded samples.
#include "doctest.h"
#include "WavDump.h"

#include <cstdio>
#include <fstream>
#include <vector>

namespace {
uint32_t Rd32(const std::vector<uint8_t>& b, size_t o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (uint32_t(b[o + 3]) << 24); }
uint16_t Rd16(const std::vector<uint8_t>& b, size_t o) { return static_cast<uint16_t>(b[o] | (b[o + 1] << 8)); }
}

TEST_CASE("WavDump writes a valid float WAV and stops at the configured length")
{
  const std::string path = "wavdump_test_output.wav";
  WavDump d;
  d.Configure(path, 6, 48000, 32, 3, 0.01); // 10 ms = 480 frames
  REQUIRE(d.enabled());

  std::vector<float> chunk(6 * 400);
  for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<float>(i) * 0.001f;
  d.Append(chunk.data(), chunk.size() * sizeof(float));  // 400 frames
  d.Append(chunk.data(), chunk.size() * sizeof(float));  // only 80 more fit
  CHECK(d.full());
  CHECK(d.bytes() == 480u * 6u * 4u);
  REQUIRE(d.Write());

  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  f.close();
  std::remove(path.c_str());
  REQUIRE(b.size() == 44u + 480u * 6u * 4u);
  CHECK(std::string(b.begin(), b.begin() + 4) == "RIFF");
  CHECK(std::string(b.begin() + 8, b.begin() + 16) == "WAVEfmt ");
  CHECK(Rd16(b, 20) == 3);        // IEEE float
  CHECK(Rd16(b, 22) == 6);        // channels
  CHECK(Rd32(b, 24) == 48000u);   // sample rate
  CHECK(Rd32(b, 28) == 48000u * 24u);
  CHECK(Rd16(b, 32) == 24);       // block align
  CHECK(Rd16(b, 34) == 32);       // bits
  CHECK(std::string(b.begin() + 36, b.begin() + 40) == "data");
  CHECK(Rd32(b, 40) == 480u * 6u * 4u);
  CHECK(Rd32(b, 4) == 36u + 480u * 6u * 4u);
  float second;
  std::memcpy(&second, &b[44 + 4], 4);
  CHECK(second == doctest::Approx(0.001f));
}

TEST_CASE("WavDump is inert when no path is configured")
{
  WavDump d;
  d.Configure("", 2, 48000, 16, 1, 5.0);
  CHECK_FALSE(d.enabled());
  const int16_t s[4] = {1, 2, 3, 4};
  d.Append(s, sizeof s);
  CHECK(d.bytes() == 0u);
  CHECK(d.Write());
}
