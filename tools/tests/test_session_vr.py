"""droiddeck-session vr (PLAN.md Loop 2 item 8): the Steam session, with the Quest's OpenXR bridge in
the environment the client and everything it starts inherit, and xrizer as the OpenVR runtime."""
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SESSION = Path(__file__).resolve().parents[1] / "linuxfs/overlay/usr/local/bin/droiddeck-session"
XRIZER_CHECK = "[ -d /usr/local/lib/xrizer ]"


def run(script, env):
    return subprocess.run(["bash", "-c", "set -u\n" + script], env=env, capture_output=True,
                          text=True, timeout=30, check=True).stdout


def vr_function(xrizer):
    match = re.search(r"^vr_environment\(\) \{\n.*?^\}\n", SESSION.read_text(), re.M | re.S)
    function = match.group(0)
    assert XRIZER_CHECK in function
    return function.replace(XRIZER_CHECK, "[ -d %s ]" % xrizer)


class SessionVrTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, True)
        self.home = self.root / "home"
        self.home.mkdir()
        self.xrizer = self.root / "xrizer"
        self.vrpath = self.home / ".config/openvr/openvrpaths.vrpath"

    def exported(self, **env):
        """What vr_environment hands the client: its exported environment afterwards."""
        out = run(vr_function(self.xrizer) + "vr_environment >/dev/null\nenv\n",
                  dict({"PATH": os.environ["PATH"], "HOME": str(self.home)}, **env))
        return dict(line.split("=", 1) for line in out.splitlines() if "=" in line)

    def exported_twice(self, **env):
        """Environment after two calls: the loader path must not duplicate."""
        out = run(vr_function(self.xrizer) + "vr_environment >/dev/null\nvr_environment >/dev/null\nenv\n",
                  dict({"PATH": os.environ["PATH"], "HOME": str(self.home)}, **env))
        return dict(line.split("=", 1) for line in out.splitlines() if "=" in line)

    def test_vr_is_taken_as_the_steam_session_and_carried_through_gamescope(self):
        line = next(l for l in SESSION.read_text().splitlines() if l.startswith('if [ "$mode" = vr ]'))
        out = run('mode=vr\n%s\necho "$mode"\nprintenv BL_VR\n' % line, {"PATH": os.environ["PATH"]})
        self.assertEqual(["steam", "1"], out.splitlines())

    def test_preselected_manifest_still_sets_socket_loader_and_xrizer(self):
        # A caller-supplied XR_RUNTIME_JSON alone must not skip the rest of the setup.
        self.xrizer.mkdir()
        env = self.exported(XR_RUNTIME_JSON="/usr/local/lib/droiddeck-xr/questbridge.json",
                            DROIDDECK_BWRAP_SOCKET="com.droiddeck.quest.bwrap")
        self.assertEqual("@com.droiddeck.quest.xr-bridge", env["DROIDDECK_XR_SOCKET"])
        self.assertEqual("/usr/local/lib/droiddeck-xr", env["LD_LIBRARY_PATH"])
        self.assertEqual({"runtime": ["/usr/local/lib/xrizer"], "version": 1, "jsonid": "vrpathreg"},
                         json.loads(self.vrpath.read_text()))

    def test_repeated_invocation_does_not_duplicate_the_loader_path(self):
        env = self.exported_twice(DROIDDECK_BWRAP_SOCKET="com.droiddeck.quest.bwrap",
                                  LD_LIBRARY_PATH="/usr/lib/extra")
        self.assertEqual("/usr/local/lib/droiddeck-xr:/usr/lib/extra", env["LD_LIBRARY_PATH"])
        self.assertEqual("@com.droiddeck.quest.xr-bridge", env["DROIDDECK_XR_SOCKET"])


    def test_the_client_gets_the_runtime_the_socket_and_the_loader(self):
        env = self.exported(DROIDDECK_BWRAP_SOCKET="com.droiddeck.quest.bwrap", LD_LIBRARY_PATH="/usr/lib/extra")
        self.assertEqual("/usr/local/lib/droiddeck-xr/questbridge.json", env["XR_RUNTIME_JSON"])
        self.assertEqual("@com.droiddeck.quest.xr-bridge", env["DROIDDECK_XR_SOCKET"])
        self.assertEqual("/usr/local/lib/droiddeck-xr:/usr/lib/extra", env["LD_LIBRARY_PATH"])

    def test_without_the_bwrap_socket_it_is_the_runtime_default(self):
        env = self.exported()
        self.assertEqual("@com.droiddeck.launcher.xr-bridge", env["DROIDDECK_XR_SOCKET"])
        self.assertEqual("/usr/local/lib/droiddeck-xr", env["LD_LIBRARY_PATH"])

    def test_openvr_titles_are_sent_to_xrizer_only_when_it_is_there(self):
        self.exported()
        self.assertFalse(self.vrpath.exists())
        self.xrizer.mkdir()
        self.exported()
        self.assertEqual({"runtime": ["/usr/local/lib/xrizer"], "version": 1, "jsonid": "vrpathreg"},
                         json.loads(self.vrpath.read_text()))

    def test_without_xrizer_an_existing_vrpath_is_left_alone(self):
        # No destructive cleanup: without xrizer a user's own registry stays as it was.
        self.vrpath.parent.mkdir(parents=True)
        self.vrpath.write_text('{"runtime": ["/user/own"], "version": 1, "jsonid": "vrpathreg"}')
        self.assertNotIn("DXVK_NO_VR", self.exported())
        self.assertEqual('{"runtime": ["/user/own"], "version": 1, "jsonid": "vrpathreg"}',
                         self.vrpath.read_text())

    def test_vr_setup_preserves_explicit_dxvk_diagnostics_without_enabling_them(self):
        self.xrizer.mkdir()
        self.assertNotIn("DXVK_NO_VR", self.exported())
        self.assertEqual("1", self.exported(DXVK_NO_VR="1")["DXVK_NO_VR"])
        self.assertEqual("0", self.exported(DXVK_NO_VR="0")["DXVK_NO_VR"])


if __name__ == "__main__":
    unittest.main()
