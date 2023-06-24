#!/usr/bin/env python3
import os
import json
import time
import unittest
import subprocess

import cereal.messaging as messaging
from system.hardware import TICI
from selfdrive.manager.process_config import managed_processes


class TestRawgpsd(unittest.TestCase):
  @classmethod
  def setUpClass(cls):
    if not TICI:
      raise unittest.SkipTest

    cls.sm = messaging.SubMaster(['qcomGnss'])

  def tearDown(self):
    managed_processes['rawgpsd'].stop()
    os.system("sudo systemctl restart ModemManager")

  def _wait_for_output(self, t=10):
    self.sm.update(0)
    for __ in range(t):
      self.sm.update(1 * 1000)
      if self.sm.updated['qcomGnss']:
        break
    return self.sm.updated['qcomGnss']

  def test_wait_for_modem(self):
    os.system("sudo systemctl stop ModemManager")
    managed_processes['rawgpsd'].start()
    assert not self._wait_for_output(5)

    os.system("sudo systemctl restart ModemManager")
    assert self._wait_for_output()

  def test_startup_time(self):
    for _ in range(5):
      managed_processes['rawgpsd'].start()

      start_time = time.monotonic()
      assert self._wait_for_output(), "rawgpsd didn't start outputting messages in time"

      et = time.monotonic() - start_time
      assert et < 5, f"rawgpsd took {et:.1f}s to start"
      managed_processes['rawgpsd'].stop()

  def test_turns_off_gnss(self):
    for s in (0.1, 0.5, 1, 5):
      managed_processes['rawgpsd'].start()
      time.sleep(s)
      managed_processes['rawgpsd'].stop()

      ls = subprocess.check_output("mmcli -m any --location-status --output-json", shell=True, encoding='utf-8')
      loc_status = json.loads(ls)
      assert set(loc_status['modem']['location']['enabled']) <= {'3gpp-lac-ci'}


if __name__ == "__main__":
  unittest.main()
