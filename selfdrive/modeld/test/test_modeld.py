#!/usr/bin/env python3
import time
import unittest
import numpy as np
import random

import cereal.messaging as messaging
from cereal.visionipc.visionipc_pyx import VisionIpcServer, VisionStreamType  # pylint: disable=no-name-in-module, import-error
from common.transformations.camera import tici_f_frame_size
from common.realtime import DT_MDL
from selfdrive.manager.process_config import managed_processes


VIPC_STREAM = {"roadCameraState": VisionStreamType.VISION_STREAM_ROAD, "driverCameraState": VisionStreamType.VISION_STREAM_DRIVER,
               "wideRoadCameraState": VisionStreamType.VISION_STREAM_WIDE_ROAD}

IMG = np.zeros(int(tici_f_frame_size[0]*tici_f_frame_size[1]*(3/2)), dtype=np.uint8)
IMG_BYTES = IMG.flatten().tobytes()

class TestModeld(unittest.TestCase):

  def setUp(self):
    self.vipc_server = VisionIpcServer("camerad")
    self.vipc_server.create_buffers(VisionStreamType.VISION_STREAM_ROAD, 40, False, *tici_f_frame_size)
    self.vipc_server.create_buffers(VisionStreamType.VISION_STREAM_DRIVER, 40, False, *tici_f_frame_size)
    self.vipc_server.create_buffers(VisionStreamType.VISION_STREAM_WIDE_ROAD, 40, False, *tici_f_frame_size)
    self.vipc_server.start_listener()

    self.sm = messaging.SubMaster(['modelV2', 'cameraOdometry'])
    self.pm = messaging.PubMaster(['roadCameraState', 'wideRoadCameraState', 'liveCalibration', 'lateralPlan'])

    managed_processes['modeld'].start()
    time.sleep(0.2)
    self.sm.update(1000)

  def tearDown(self):
    managed_processes['modeld'].stop()
    del self.vipc_server

  def _send_frames(self, frame_id, cams=None):
    if cams is None:
      cams = ('roadCameraState', 'wideRoadCameraState')

    cs = None
    for cam in cams:
      msg = messaging.new_message(cam)
      cs = getattr(msg, cam)
      cs.frameId = frame_id
      cs.timestampSof = int((frame_id * DT_MDL) * 1e9)
      cs.timestampEof = int(cs.timestampSof + (DT_MDL * 1e9))

      self.pm.send(msg.which(), msg)
      self.vipc_server.send(VIPC_STREAM[msg.which()], IMG_BYTES, cs.frameId,
                            cs.timestampSof, cs.timestampEof)
    return cs

  def _wait(self):
    self.sm.update(5000)
    if self.sm['modelV2'].frameId != self.sm['cameraOdometry'].frameId:
      self.sm.update(1000)

  def test_modeld(self):
    for n in range(1, 500):
      cs = self._send_frames(n)
      self._wait()

      mdl = self.sm['modelV2']
      self.assertEqual(mdl.frameId, n)
      self.assertEqual(mdl.frameIdExtra, n)
      self.assertEqual(mdl.timestampEof, cs.timestampEof)
      self.assertEqual(mdl.frameAge, 0)
      self.assertEqual(mdl.frameDropPerc, 0)

      odo = self.sm['cameraOdometry']
      self.assertEqual(odo.frameId, n)
      self.assertEqual(odo.timestampEof, cs.timestampEof)

  def test_dropped_frames(self):
    """
      modeld should only run on consecutive frames
    """
    for n in range(1, 50):
      skip = (n % 10) == 0

      if skip:
        cams = random.choice([(), ('wideRoadCameraState', )])
        self._send_frames(n, cams)
      else:
        self._send_frames(n)
      self._wait()

      frame_id = n-1 if skip else n
      mdl = self.sm['modelV2']
      odo = self.sm['cameraOdometry']
      self.assertEqual(mdl.frameId, frame_id)
      self.assertEqual(mdl.frameIdExtra, frame_id)
      self.assertEqual(odo.frameId, frame_id)
      if skip:
        self.assertFalse(self.sm.updated['modelV2'])
        self.assertFalse(self.sm.updated['cameraOdometry'])


if __name__ == "__main__":
  unittest.main()
