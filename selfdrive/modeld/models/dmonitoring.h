#pragma once

#include <vector>

#include "cereal/messaging/messaging.h"
#include "selfdrive/common/util.h"
#include "selfdrive/modeld/models/commonmodel.h"
#include "selfdrive/modeld/runners/run.h"

#define OUTPUT_SIZE 38

typedef struct DMonitoringResult {
  float face_orientation[3];
  float face_orientation_meta[3];
  float face_position[2];
  float face_position_meta[2];
  float face_prob;
  float left_eye_prob;
  float right_eye_prob;
  float left_blink_prob;
  float right_blink_prob;
  float sg_prob;
  float poor_vision;
  float partial_face;
  float distracted_pose;
  float distracted_eyes;
  float dsp_execution_time;
} DMonitoringResult;

struct Rect {
  int x, y, w, h;
};

class YUVBuf {
 public:
  YUVBuf(int w, int h) : width(w), height(h) {
    buf = new uint8_t[w * h * 3 / 2];
    y = buf;
    u = y + width * height;
    v = u + (width / 2) * (height / 2);
  }
  ~YUVBuf() {
    delete[] buf;
  }
  uint8_t *y, *u, *v;
  int width, height;
  uint8_t* buf;
};

typedef struct DMonitoringModelState {
  RunModel *m;
  bool is_rhd;
  float output[OUTPUT_SIZE];
  std::unique_ptr<YUVBuf> resized_buf;
  std::unique_ptr<YUVBuf> cropped_buf;
  std::unique_ptr<YUVBuf> premirror_cropped_buf;
  std::vector<float> net_input_buf;
  float tensor[UINT8_MAX + 1];
  Rect crop_rect;
} DMonitoringModelState;

void dmonitoring_init(DMonitoringModelState* s, int width, int height);
DMonitoringResult dmonitoring_eval_frame(DMonitoringModelState* s, void* stream_buf, int width, int height);
void dmonitoring_publish(PubMaster &pm, uint32_t frame_id, const DMonitoringResult &res, float execution_time, kj::ArrayPtr<const float> raw_pred);
void dmonitoring_free(DMonitoringModelState* s);

