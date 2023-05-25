#!/usr/bin/env python3
import random
import unittest

import numpy as np

import cereal.messaging as messaging
from cereal import log
from common.params import Params
from selfdrive.locationd.calibrationd import Calibrator, INPUTS_NEEDED, INPUTS_WANTED, BLOCK_SIZE, MIN_SPEED_FILTER, MAX_YAW_RATE_FILTER, SMOOTH_CYCLES


class TestCalibrationd(unittest.TestCase):

  def test_read_saved_params(self):
    msg = messaging.new_message('liveCalibration')
    msg.liveCalibration.validBlocks = random.randint(1, 10)
    msg.liveCalibration.rpyCalib = [random.random() for _ in range(3)]
    Params().put("CalibrationParams", msg.to_bytes())
    c = Calibrator(param_put=True)

    np.testing.assert_allclose(msg.liveCalibration.rpyCalib, c.rpy)
    self.assertEqual(msg.liveCalibration.validBlocks, c.valid_blocks)


  def test_calibration_basics(self):
    c = Calibrator(param_put=False)
    for _ in range(BLOCK_SIZE * INPUTS_WANTED):
      c.handle_v_ego(MIN_SPEED_FILTER + 1)
      c. handle_cam_odom([MIN_SPEED_FILTER + 1, 0.0, 0.0],
                         [0.0, 0.0, 0.0],
                         [0.0, 0.0, 0.0],
                         [1e-3, 1e-3, 1e-3])
    self.assertEqual(c.valid_blocks, INPUTS_WANTED)
    np.testing.assert_allclose(c.rpy, np.zeros(3))
    c.reset()

  def test_calibration_ref(self):
    ref_rpy = np.array([2.30151750e-06, -8.63402487e-04, -2.86042531e-03])
    ref_valid_blocks = 13

    np.random.seed(0)
    c = Calibrator(param_put=False)
    for _ in range(BLOCK_SIZE * INPUTS_WANTED):
      trans = [np.random.rand() * MIN_SPEED_FILTER * 3, np.random.randn() / 10, np.random.randn() / 10]
      rot = [np.random.randn() / 10, np.random.randn() / 10, np.random.rand() * MAX_YAW_RATE_FILTER * 2]
      trans_std = [np.random.randn() / 10, np.random.randn() / 10, np.random.randn() / 10]
      c.handle_v_ego(trans[0])
      c.handle_cam_odom(trans,
                        rot,
                        [0.0, 0.0, 0.0],
                        trans_std)
    np.testing.assert_allclose(c.rpy, ref_rpy)
    np.testing.assert_allclose(c.valid_blocks, ref_valid_blocks)
    c.reset()

  def test_calibration_low_speed_reject(self):
    c = Calibrator(param_put=False)
    for _ in range(BLOCK_SIZE * INPUTS_WANTED):
      c.handle_v_ego(MIN_SPEED_FILTER - 1)
      c. handle_cam_odom([MIN_SPEED_FILTER + 1, 0.0, 0.0],
                         [0.0, 0.0, 0.0],
                         [0.0, 0.0, 0.0],
                         [1e-3, 1e-3, 1e-3])
    for _ in range(BLOCK_SIZE * INPUTS_WANTED):
      c.handle_v_ego(MIN_SPEED_FILTER + 1)
      c. handle_cam_odom([MIN_SPEED_FILTER - 1, 0.0, 0.0],
                         [0.0, 0.0, 0.0],
                         [0.0, 0.0, 0.0],
                         [1e-3, 1e-3, 1e-3])
    self.assertEqual(c.valid_blocks, 0)
    np.testing.assert_allclose(c.rpy, np.zeros(3))


  def test_calibration_yaw_rate_reject(self):
    c = Calibrator(param_put=False)
    for _ in range(BLOCK_SIZE * INPUTS_WANTED):
      c.handle_v_ego(MIN_SPEED_FILTER + 1)
      c. handle_cam_odom([MIN_SPEED_FILTER + 1, 0.0, 0.0],
                         [0.0, 0.0, MAX_YAW_RATE_FILTER ],
                         [0.0, 0.0, 0.0],
                         [1e-3, 1e-3, 1e-3])
    self.assertEqual(c.valid_blocks, 0)
    np.testing.assert_allclose(c.rpy, np.zeros(3))


  def test_calibration_speed_std_reject(self):
    c = Calibrator(param_put=False)
    for _ in range(BLOCK_SIZE * INPUTS_WANTED):
      c.handle_v_ego(MIN_SPEED_FILTER + 1)
      c. handle_cam_odom([MIN_SPEED_FILTER + 1, 0.0, 0.0],
                         [0.0, 0.0, 0.0],
                         [0.0, 0.0, 0.0],
                         [1e3, 1e3, 1e3])
    self.assertEqual(c.valid_blocks, INPUTS_NEEDED)
    np.testing.assert_allclose(c.rpy, np.zeros(3))


  def test_calibration_auto_reset(self):
    c = Calibrator(param_put=False)
    for _ in range(BLOCK_SIZE * INPUTS_WANTED):
      c.handle_v_ego(MIN_SPEED_FILTER + 1)
      c. handle_cam_odom([MIN_SPEED_FILTER + 1, 0.0, 0.0],
                         [0.0, 0.0, 0.0],
                         [0.0, 0.0, 0.0],
                         [1e-3, 1e-3, 1e-3])
    self.assertEqual(c.valid_blocks, INPUTS_WANTED)
    np.testing.assert_allclose(c.rpy, [0.0, 0.0, 0.0])
    old_rpy_weight_prev = 0.0
    for _ in range(BLOCK_SIZE + 10):
      self.assertLess(old_rpy_weight_prev - c.old_rpy_weight, 1/SMOOTH_CYCLES + 1e-3)
      old_rpy_weight_prev = c.old_rpy_weight
      c.handle_v_ego(MIN_SPEED_FILTER + 1)
      c.handle_cam_odom([MIN_SPEED_FILTER + 1, -0.05 * MIN_SPEED_FILTER, 0.0],
                         [0.0, 0.0, 0.0],
                         [0.0, 0.0, 0.0],
                         [1e-3, 1e-3, 1e-3])
    self.assertEqual(c.valid_blocks, 1)
    self.assertEqual(c.cal_status, log.LiveCalibrationData.Status.recalibrating)
    np.testing.assert_allclose(c.rpy, [0.0, 0.0, -0.05], atol=1e-2)

if __name__ == "__main__":
  unittest.main()
