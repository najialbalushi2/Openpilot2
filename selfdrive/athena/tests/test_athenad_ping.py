#!/usr/bin/env python3
import subprocess
import threading
import time
import unittest
from typing import cast, Optional
from unittest import mock
from unittest.mock import MagicMock

from openpilot.common.params import Params
from openpilot.common.timeout import Timeout
from openpilot.selfdrive.athena import athenad
from openpilot.selfdrive.manager.helpers import write_onroad_params
from openpilot.system.hardware import TICI
from websocket import (ABNF, WebSocket, WebSocketException, WebSocketTimeoutException,
                       create_connection)


def wifi_radio(on: bool) -> None:
  if not TICI:
    return
  print(f"wifi {'on' if on else 'off'}")
  subprocess.run(["nmcli", "radio", "wifi", "on" if on else "off"], check=True)


class TestAthenadPing(unittest.TestCase):
  params: Params
  dongle_id: str

  athenad: threading.Thread
  exit_event: threading.Event

  def _get_ping_time(self) -> Optional[str]:
    return cast(Optional[str], self.params.get("LastAthenaPingTime", encoding="utf-8"))

  def _clear_ping_time(self) -> None:
    self.params.remove("LastAthenaPingTime")

  def _received_ping(self) -> bool:
    return self._get_ping_time() is not None

  @classmethod
  def tearDownClass(cls) -> None:
    wifi_radio(True)

  def setUp(self) -> None:
    self.params = Params()
    self.dongle_id = self.params.get("DongleId", encoding="utf-8")

    wifi_radio(True)
    self._clear_ping_time()

    self.exit_event = threading.Event()
    self.athenad = threading.Thread(target=athenad.main, args=(self.exit_event,))

  def tearDown(self) -> None:
    if self.athenad.is_alive():
      self.exit_event.set()
      self.athenad.join()

  @mock.patch('openpilot.selfdrive.athena.athenad.create_connection', new_callable=lambda: MagicMock(wraps=athenad.create_connection))
  # @mock.patch('openpilot.selfdrive.athena.athenad.create_connection', new=MagicMock(wraps=create_connection))
  def assertTimeout(self, reconnect_time: float, mock_create_connection) -> None:
    print(athenad.create_connection.call_count)
    # mock_create_connection.side_effect = create_connection
    self.athenad.start()
    print(mock_create_connection.call_count)

    time.sleep(1)
    mock_create_connection.assert_called_once()
    mock_create_connection.reset_mock()
    print(mock_create_connection.call_count)

    # check normal behaviour
    with self.subTest("Wi-Fi: receives ping"), Timeout(70, "no ping received"):
      while not self._received_ping():
        time.sleep(0.1)
      print("ping received")
    print(mock_create_connection.call_count)

    mock_create_connection.assert_not_called()
    return

    # websocket should attempt reconnect after short time
    with self.subTest("LTE: attempt reconnect"):
      wifi_radio(False)
      print("waiting for reconnect attempt")
      start_time = time.monotonic()
      with Timeout(reconnect_time, "no reconnect attempt"):
        while not mock_create_connection.called:
          time.sleep(0.1)
        print(f"reconnect attempt after {time.monotonic() - start_time:.2f}s")

    self._clear_ping_time()

    # check ping received after reconnect
    with self.subTest("LTE: receives ping"), Timeout(70, "no ping received"):
      while not self._received_ping():
        time.sleep(0.1)
      print("ping received")

  @unittest.skipIf(not TICI, "only run on desk")
  def test_offroad(self) -> None:
    write_onroad_params(False, self.params)
    self.assertTimeout(100)  # expect approx 90s

  # @unittest.skipIf(not TICI, "only run on desk")
  def test_onroad(self) -> None:
    write_onroad_params(True, self.params)
    self.assertTimeout(30)  # expect 20-30s


if __name__ == "__main__":
  unittest.main()
