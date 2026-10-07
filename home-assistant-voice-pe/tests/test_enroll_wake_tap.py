"""Guard guided-enrollment wake_tap capture (mirrors Sat1 voice_assistant_websocket)."""
from pathlib import Path
import unittest

COMP = Path(__file__).resolve().parents[1] / "esphome/components/voice_assistant_websocket"


class EnrollWakeTap(unittest.TestCase):
    def setUp(self):
        self.cpp = (COMP / "voice_assistant_websocket.cpp").read_text()
        self.h = (COMP / "voice_assistant_websocket.h").read_text()

    def test_header(self):
        self.assertIn('#include "wake_capture_sample.h"', self.h)
        self.assertIn("std::atomic<bool> enroll_wake_tap_{false};", self.h)
        self.assertIn("void enter_enrollment_(bool wake_tap);", self.h)

    def test_start_frame_parses_capture_and_acks(self):
        self.assertIn('this->enter_enrollment_(wake_tap);', self.cpp)
        self.assertIn('\\"type\\":\\"enroll_capture\\"', self.cpp)
        self.assertIn("pdMS_TO_TICKS(50)", self.cpp)

    def test_mic_loop_uses_wake_channel(self):
        self.assertIn("this->enrolling_ && this->enroll_wake_tap_", self.cpp)
        self.assertIn("wake_capture_sample(sample, this->enrollment_wake_gain_factor_)", self.cpp)


if __name__ == "__main__":
    unittest.main()
