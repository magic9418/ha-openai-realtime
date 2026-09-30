"""Guard the PE-specific assistant handoff (separate from the shared transport)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DropInHandoff(unittest.TestCase):
    def test_assistant_queues_validated_instruction_and_waits_for_release(self):
        source = (ROOT / "esphome/components/voice_assistant_websocket/voice_assistant_websocket.cpp").read_text()
        handler = source.split('std::strcmp(type, "drop_in")', 1)[1].split('#else', 1)[0]
        self.assertIn('room.size() <= 64', handler)
        self.assertIn('request_id.size() <= 128', handler)
        self.assertIn('pending_dropin_deadline_ms_ = millis() + 5000', handler)
        self.assertIn('this->stop();', handler)
        loop = source.split('void VoiceAssistantWebSocket::loop()', 1)[1].split('// Handle pending reboot', 1)[0]
        self.assertIn('websocket_client_ == nullptr', loop)
        self.assertIn('speaker_->is_stopped()', loop)
        self.assertIn('dropin_source_ready()', loop)
        self.assertIn('request_id.swap(this->pending_dropin_request_id_)', loop)
        self.assertIn('request_drop_in_room(room, request_id)', loop)


if __name__ == '__main__':
    unittest.main()
