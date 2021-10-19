#pragma once

// gate this here
#define TEMPORAL
#define DESIRE
#define TRAFFIC_CONVENTION

#include <array>
#include <memory>

#include "cereal/messaging/messaging.h"
#include "selfdrive/common/mat.h"
#include "selfdrive/common/modeldata.h"
#include "selfdrive/common/util.h"
#include "selfdrive/modeld/models/commonmodel.h"
#include "selfdrive/modeld/runners/run.h"

constexpr int PLAN_MHP_N = 5;

constexpr int DESIRE_LEN = 8;
constexpr int TRAFFIC_CONVENTION_LEN = 2;
constexpr int MODEL_FREQ = 20;

template <size_t size>
struct ModelDataXYZPivot {
  std::array<float, size> x = {};
  std::array<float, size> y = {};
  std::array<float, size> z = {};
};

struct ModelDataPlanTimestepPivot {
  ModelDataXYZPivot<TRAJECTORY_SIZE> mean = {};
  ModelDataXYZPivot<TRAJECTORY_SIZE> std_exp = {};
};

struct ModelDataRawPlanPredictionPivot {
  ModelDataPlanTimestepPivot position = {};
  ModelDataPlanTimestepPivot velocity = {};
  ModelDataPlanTimestepPivot acceleration = {};
  ModelDataPlanTimestepPivot rotation = {};
  ModelDataPlanTimestepPivot rotation_rate = {};
};

struct ModelDataRawXYZ {
  float x;
  float y;
  float z;

  // inline avoids copying struct when returning it
  // inline can be changed to constexpr when c2 deprecated
  inline const ModelDataRawXYZ to_exp() const {
    return ModelDataRawXYZ {.x=exp(x), .y=exp(y), .z=exp(z)};
  };
};
static_assert(sizeof(ModelDataRawXYZ) == sizeof(float)*3);

struct ModelDataRawPlanTimeStep {
  ModelDataRawXYZ position;
  ModelDataRawXYZ velocity;
  ModelDataRawXYZ acceleration;
  ModelDataRawXYZ rotation;
  ModelDataRawXYZ rotation_rate;
};
static_assert(sizeof(ModelDataRawPlanTimeStep) == sizeof(ModelDataRawXYZ)*5);

struct ModelDataRawPlanPrediction {
  std::array<ModelDataRawPlanTimeStep, TRAJECTORY_SIZE> mean;
  std::array<ModelDataRawPlanTimeStep, TRAJECTORY_SIZE> std;
  float prob;

  // inline avoids copying array when returning it
  // inline can be changed to constexpr when c2 deprecated
  inline const ModelDataRawPlanPredictionPivot pivot() const {
    ModelDataRawPlanPredictionPivot data = {};
    for(int i=0; i<TRAJECTORY_SIZE; i++) {
      data.position.mean.x[i] = mean[i].position.x;
      data.position.mean.y[i] = mean[i].position.y;
      data.position.mean.z[i] = mean[i].position.z;
      data.velocity.mean.x[i] = mean[i].velocity.x;
      data.velocity.mean.y[i] = mean[i].velocity.y;
      data.velocity.mean.z[i] = mean[i].velocity.z;
      data.acceleration.mean.x[i] = mean[i].acceleration.x;
      data.acceleration.mean.y[i] = mean[i].acceleration.y;
      data.acceleration.mean.z[i] = mean[i].acceleration.z;
      data.rotation.mean.x[i] = mean[i].rotation.x;
      data.rotation.mean.y[i] = mean[i].rotation.y;
      data.rotation.mean.z[i] = mean[i].rotation.z;
      data.rotation_rate.mean.x[i] = mean[i].rotation_rate.x;
      data.rotation_rate.mean.y[i] = mean[i].rotation_rate.y;
      data.rotation_rate.mean.z[i] = mean[i].rotation_rate.z;

      auto position_std_exp = std[i].position.to_exp();
      data.position.std_exp.x[i] = position_std_exp.x;
      data.position.std_exp.y[i] = position_std_exp.y;
      data.position.std_exp.z[i] = position_std_exp.z;
    }
    return data;
  }
  // inline const ModelDataXYZPivot<TRAJECTORY_SIZE> pivot(const std::array<ModelDataRawPlanTimeStep, TRAJECTORY_SIZE> &arr,
  //                                                       std::function<const ModelDataRawXYZ&(const ModelDataRawPlanTimeStep&)> func,
  //                                                       bool to_exp=false) const {
  //   ModelDataXYZPivot<TRAJECTORY_SIZE> data = {};
  //   for(int i=0; i<TRAJECTORY_SIZE; i++) {
  //     ModelDataRawXYZ d = to_exp ? func(arr[i]).to_exp() : func(arr[i]);
  //     data.x[i] = d.x;
  //     data.y[i] = d.y;
  //     data.z[i] = d.z;
  //   }
  //   return data;
  // }
};
static_assert(sizeof(ModelDataRawPlanPrediction) == (sizeof(ModelDataRawPlanTimeStep)*TRAJECTORY_SIZE*2) + sizeof(float));

struct ModelDataRawPlan {
  std::array<ModelDataRawPlanPrediction, PLAN_MHP_N> prediction;

  constexpr const ModelDataRawPlanPrediction &best_prediction() const {
    int max_idx = 0;
    for (int i = 1; i < prediction.size(); i++) {
      if (prediction[i].prob > prediction[max_idx].prob) {
        max_idx = i;
      }
    }
    return prediction[max_idx];
  }
};
static_assert(sizeof(ModelDataRawPlan) == sizeof(ModelDataRawPlanPrediction)*PLAN_MHP_N);

struct ModelDataRawPose {
  ModelDataRawXYZ velocity_mean;
  ModelDataRawXYZ rotation_mean;
  ModelDataRawXYZ velocity_std;
  ModelDataRawXYZ rotation_std;
};
static_assert(sizeof(ModelDataRawPose) == sizeof(ModelDataRawXYZ)*4);

struct ModelDataRaw {
  ModelDataRawPlan *plan;
  float *lane_lines;
  float *lane_lines_prob;
  float *road_edges;
  float *lead;
  float *lead_prob;
  float *desire_state;
  float *meta;
  float *desire_pred;
  ModelDataRawPose *pose;
};

typedef struct ModelState {
  ModelFrame *frame;
  std::vector<float> output;
  std::unique_ptr<RunModel> m;
#ifdef DESIRE
  float prev_desire[DESIRE_LEN] = {};
  float pulse_desire[DESIRE_LEN] = {};
#endif
#ifdef TRAFFIC_CONVENTION
  float traffic_convention[TRAFFIC_CONVENTION_LEN] = {};
#endif
} ModelState;

void model_init(ModelState* s, cl_device_id device_id, cl_context context);
ModelDataRaw model_eval_frame(ModelState* s, cl_mem yuv_cl, int width, int height,
                           const mat3 &transform, float *desire_in);
void model_free(ModelState* s);
void poly_fit(float *in_pts, float *in_stds, float *out);
void model_publish(PubMaster &pm, uint32_t vipc_frame_id, uint32_t frame_id, float frame_drop,
                   const ModelDataRaw &net_outputs, uint64_t timestamp_eof,
                   float model_execution_time, kj::ArrayPtr<const float> raw_pred);
void posenet_publish(PubMaster &pm, uint32_t vipc_frame_id, uint32_t vipc_dropped_frames,
                     const ModelDataRaw &net_outputs, uint64_t timestamp_eof);
