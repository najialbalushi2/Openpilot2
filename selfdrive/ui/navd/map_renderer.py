#!/usr/bin/env python3
# You might need to uninstall the PyQt5 pip package to avoid conflicts

import os
import time
from cffi import FFI

from common.ffi_wrapper import suffix
from common.basedir import BASEDIR

HEIGHT = WIDTH = 256


def get_ffi():
  lib = os.path.join(BASEDIR, "selfdrive", "ui", "navd", "libmap_renderer" + suffix())

  ffi = FFI()
  ffi.cdef("""
void* map_renderer_init();
void map_renderer_update_position(void *inst, float lat, float lon, float bearing);
void map_renderer_update(void *inst);
void map_renderer_process(void *inst);
bool map_renderer_loaded(void *inst);
uint8_t* map_renderer_get_image(void *inst);
void map_renderer_free_image(void *inst, uint8_t *buf);
""")
  return ffi, ffi.dlopen(lib)


def wait_ready(lib, renderer):
  while not lib.map_renderer_loaded(renderer):
    lib.map_renderer_update(renderer)

    # The main qt app is not execed, so we need to periodically process events for e.g. network requests
    lib.map_renderer_process(renderer)

    time.sleep(0.01)


def get_image(lib, renderer):
  buf = lib.map_renderer_get_image(renderer)
  r = list(buf[0:3 * WIDTH * HEIGHT])
  lib.map_renderer_free_image(renderer, buf)

  # Convert to numpy
  r = np.asarray(r)
  return r.reshape((WIDTH, HEIGHT, 3))


if __name__ == "__main__":
  import matplotlib.pyplot as plt
  import numpy as np

  ffi, lib = get_ffi()
  renderer = lib.map_renderer_init()

  POSITIONS = [
    (32.71569271952601, -117.16384270868463, 0), (32.71569271952601, -117.16384270868463, 45),  # San Diego
    (52.378641991483136, 4.902623379456488, 0), (52.378641991483136, 4.902623379456488, 45),  # Amsterdam
  ]
  plt.figure()

  for i, pos in enumerate(POSITIONS):
    t = time.time()
    lib.map_renderer_update_position(renderer, *pos)
    wait_ready(lib, renderer)

    print(f"{pos} took {time.time() - t:.2f} s")

    plt.subplot(2, 2, i + 1)
    plt.imshow(get_image(lib, renderer))

  plt.show()
