#include <opus.h>
#include <opusenc.h>

#include <string>

#include "core/types.h"
#include "core/version.h"
#include "test.h"

TEST(smoke_opus_links) {
  CHECK(std::string(opus_get_version_string()).find("libopus 1.5") != std::string::npos);
  CHECK(ope_get_version_string() != nullptr);
}

TEST(types_and_version) {
  spool::StreamFormat a{48000, 2}, b{48000, 2}, c{44100, 2};
  CHECK(a == b);
  CHECK(!(a == c));
  CHECK_EQ(std::string(spool::kVersion), std::string("0.1.0"));
}
