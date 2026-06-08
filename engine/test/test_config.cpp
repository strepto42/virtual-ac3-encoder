// test_config.cpp — behavioral defaults of the engine Config.
#include "doctest.h"
#include "Config.h"

// With no config file and no flags, stereo sources should be upmixed to 5.1 by default
// (this tool targets a 5.1 optical path, so using all speakers is the sensible default).
TEST_CASE("config default: surround upmix is on by default")
{
  CHECK(Config{}.upmix == "surround");
}
