import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "weclawbot_test",
    ROOT / "__init__.py",
    submodule_search_locations=[str(ROOT)],
)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules["weclawbot_test"] = MODULE
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)

from weclawbot_test.mqtt_control import _connect_packet, _publish_packet


class MqttCodecTest(unittest.TestCase):
    def test_connect_packet_is_mqtt_311_clean_session(self):
        packet = _connect_packet("client", "user", "password")
        self.assertEqual(packet[0], 0x10)
        self.assertIn(b"MQTT\x04\xc2", packet)

    def test_publish_packet_uses_qos_one(self):
        packet = _publish_packet("weclawbot/v1/devices/demo/control", b"{}")
        self.assertEqual(packet[0], 0x32)


if __name__ == "__main__":
    unittest.main()
