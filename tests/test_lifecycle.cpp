#include <cstddef>

#include "app/lifecycle.h"
#include "core/sample_ring.h"
#include "test.h"

using namespace sl;
using sl::CaptureConfig;
using sl::CaptureMode;

namespace {
constexpr size_t kMiB = size_t(1) << 20;
}

TEST(lifecycle_working_set_for_ring) {
  // 30 s at 48 kHz stereo: 1,440,000 frames round up to 2^21, 8 bytes each = 16 MiB.
  const WorkingSetSize w = working_set_for_ring(30, 48000, 2);
  CHECK_EQ(w.min_bytes, (16 + 64) * kMiB);
  CHECK_EQ(w.max_bytes, (16 + 64 + 256) * kMiB);
  // Like the session: at least 1 s and 1 channel (48000 frames -> 2^16, 4 bytes each).
  CHECK_EQ(working_set_for_ring(0, 48000, 0).min_bytes, (size_t(1) << 16) * 4 + 64 * kMiB);
  // The largest settings: 3600 s at 192 kHz, 8 ch -> 2^30 frames of 32 bytes = 32 GiB.
  CHECK_EQ(working_set_for_ring(3600, 192000, 8).min_bytes, (size_t(32) << 30) + 64 * kMiB);
}

// Without the raise, a 16 MiB ring cannot be VirtualLock'ed under the default quota.
TEST(lifecycle_raised_working_set_lets_ring_lock) {
  CHECK(raise_working_set(working_set_for_ring(30, 48000, 2)));
  SampleRing ring(2, 30 * 48000);
  CHECK_EQ(ring.capacity(), size_t(1) << 21);
  CHECK(ring.locked());
}

// R19: a crashed session resumes only on the device it was recording from.
TEST(lifecycle_resume_input_pins_device) {
  CaptureConfig in;
  in.mode = CaptureMode::Exclusive;
  in.sample_rate = 96000;  // device_id empty: follow the Windows default
  RecordingState st;
  st.device_id = L"{pinned}";
  const CaptureConfig c = resume_input(in, st);
  CHECK(c.device_id == L"{pinned}");
  CHECK(c.mode == CaptureMode::Exclusive);
  CHECK_EQ(c.sample_rate, 96000u);
  st.device_id.clear();  // synthetic, or a state from before R19: the settings as they are
  CHECK(resume_input(in, st).device_id.empty());

  // The open source must be that device; a different (default) one is reopened.
  CHECK(!resume_needs_open(L"{pinned}", true, L"{pinned}"));
  CHECK(resume_needs_open(L"{other}", true, L"{pinned}"));
  CHECK(resume_needs_open(L"{pinned}", false, L"{pinned}"));  // not open
  CHECK(!resume_needs_open(L"", true, L""));                  // nothing pinned: as now
  CHECK(resume_needs_open(L"", false, L""));
}
