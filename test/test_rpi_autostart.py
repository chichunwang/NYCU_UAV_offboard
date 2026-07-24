from pathlib import Path
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEPLOY_ROOT = PROJECT_ROOT / "deploy" / "rpi"


class RpiAutostartStaticTest(unittest.TestCase):
    def read(self, relative_path):
        return (DEPLOY_ROOT / relative_path).read_text(encoding="utf-8")

    def test_runtime_scripts_are_lf_bash_scripts(self):
        for name in ("common.sh", "start-dds.sh", "start-topics.sh"):
            data = (DEPLOY_ROOT / "runtime" / name).read_bytes()
            self.assertTrue(data.startswith(b"#!/usr/bin/env bash\n"), name)
            self.assertNotIn(b"\r\n", data, name)
            self.assertIn(b"set -Eeuo pipefail", data, name)

    def test_both_serial_devices_are_waited_for(self):
        dds = self.read("runtime/start-dds.sh")
        topics = self.read("runtime/start-topics.sh")
        self.assertIn('wait_for_serial_device "Pixhawk" "$PIXHAWK_SERIAL"', dds)
        self.assertIn('wait_for_serial_device "LR24" "$LR24_SERIAL"', topics)

    def test_topics_service_starts_safe_gps_goto_launch(self):
        topics = self.read("runtime/start-topics.sh")
        self.assertIn(
            "ros2 launch my_offboard_cpp serial_gps_goto.launch.py", topics
        )
        self.assertNotIn("serial_elrs_offboard.launch.py", topics)

        launch = (
            PROJECT_ROOT / "launch" / "serial_gps_goto.launch.py"
        ).read_text(encoding="utf-8")
        self.assertNotIn("serial_elrs_offboard.launch.py", launch)
        self.assertNotIn("my_offboard_node", launch)
        self.assertEqual(launch.count("respawn=True"), 2)

    def test_lr24_node_reconnects_serial_without_manual_restart(self):
        source = (PROJECT_ROOT / "src" / "lr24_command_node.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("kSerialReconnectDelay = 2s", source)
        self.assertIn("close_serial_and_schedule_reconnect()", source)
        self.assertIn("check_serial_device_identity()", source)
        self.assertIn("schedule_serial_reconnect();", source)

    def test_services_run_unprivileged_and_restart(self):
        for name in ("nycu-uav-dds@.service", "nycu-uav-topics@.service"):
            path = DEPLOY_ROOT / "systemd" / name
            self.assertNotIn(b"\r\n", path.read_bytes(), name)
            unit = path.read_text(encoding="utf-8")
            self.assertIn("User=%i", unit)
            self.assertIn("SupplementaryGroups=dialout", unit)
            self.assertIn("Restart=always", unit)
            self.assertIn("StartLimitIntervalSec=0", unit)
            self.assertIn("UMask=0027", unit)

    def test_target_starts_both_services(self):
        target = self.read("systemd/nycu-uav-offboard@.target")
        self.assertIn("nycu-uav-dds@%i.service", target)
        self.assertIn("nycu-uav-topics@%i.service", target)
        self.assertIn("WantedBy=multi-user.target", target)

    def test_installer_installs_every_runtime_and_unit_file(self):
        installer = self.read("install.sh")
        for name in (
            "common.sh",
            "start-dds.sh",
            "start-topics.sh",
            "nycu-uav-dds@.service",
            "nycu-uav-topics@.service",
            "nycu-uav-offboard@.target",
        ):
            self.assertIn(name, installer)

        self.assertIn("systemctl disable --now", installer)
        self.assertIn('systemctl stop "$desired_target"', installer)
        self.assertIn("systemd-analyze verify", installer)
        self.assertIn('/bin/bash -n "$shell_file"', installer)
        self.assertIn("ros2 pkg prefix my_offboard_cpp", installer)
        self.assertIn("serial_gps_goto.launch.py --show-args", installer)


if __name__ == "__main__":
    unittest.main()
