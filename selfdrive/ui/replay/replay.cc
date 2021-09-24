#include "selfdrive/ui/replay/replay.h"

#include <QApplication>

#include "cereal/services.h"
#include "selfdrive/camerad/cameras/camera_common.h"
#include "selfdrive/common/timing.h"
#include "selfdrive/hardware/hw.h"

Replay::Replay(QString route, QStringList allow, QStringList block, SubMaster *sm_, QObject *parent) : sm(sm_), QObject(parent) {
  std::vector<const char*> s;
  for (const auto &it : services) {
    if ((allow.size() == 0 || allow.contains(it.name)) &&
        !block.contains(it.name)) {
      s.push_back(it.name);
      socks.insert(it.name);
    }
  }
  qDebug() << "services " << s;

  if (sm == nullptr) {
    pm = new PubMaster(s);
  }

  route_ = std::make_unique<Route>(route);
  events_ = new std::vector<Event *>();
  // queueSegment is always executed in the main thread
  connect(this, &Replay::segmentChanged, this, &Replay::queueSegment);
}

Replay::~Replay() {
  // TODO: quit stream thread and free resources.
}

void Replay::start(int seconds){
  // load route
  if (!route_->load() || route_->size() == 0) {
    qDebug() << "failed load route" << route_->name() << "from server";
    return;
  }

  qDebug() << "load route" << route_->name() << route_->size() << "segments, start from" << seconds;
  segments_.resize(route_->size());
  seekTo(seconds);

  // start stream thread
  thread = new QThread;
  QObject::connect(thread, &QThread::started, [=]() { stream(); });
  thread->start();
}

void Replay::updateEvents(const std::function<void()>& lambda) {
  // set updating_events to true to force stream thread relase the lock and wait for evnets_udpated.
  updating_events_ = true;
  {
    std::unique_lock lk(lock_);
    lambda();
    updating_events_ = false;
    events_updated_ = true;
  }
  stream_cv_.notify_one();
}

void Replay::seekTo(int seconds, bool relative) {
  if (segments_.empty()) return;

  updateEvents([&]() {
    if (relative) {
      seconds += ((cur_mono_time_ - route_start_ts_) * 1e-9);
    }
    seconds = std::clamp(seconds, 0, (int)segments_.size() * 60 - 1);
    cur_mono_time_ = route_start_ts_ + seconds * 1e9;
    setCurrentSegment(seconds / 60);
    qInfo() << "seeking to " << seconds;
  });
}

void Replay::pause(bool pause) {
  updateEvents([=]() {
    qDebug() << (pause ? "paused..." : "resuming");
    paused_ = pause;
  });
}

void Replay::setCurrentSegment(int n) {
  if (current_segment.exchange(n) != n) {
    emit segmentChanged(n);
  }
}

// maintain the segment window
void Replay::queueSegment() {
  assert(QThread::currentThreadId() == qApp->thread()->currentThreadId());

  // fetch segments forward
  int cur_seg = current_segment.load();
  int end_idx = cur_seg;
  for (int i = cur_seg, fwd = 0; i < segments_.size() && fwd <= FORWARD_SEGS; ++i) {
    if (!segments_[i]) {
      segments_[i] = std::make_unique<Segment>(i, route_->at(i));
      QObject::connect(segments_[i].get(), &Segment::loadFinished, this, &Replay::queueSegment);
    }
    end_idx = i;
    // skip invalid segment
    fwd += segments_[i]->isValid();
  }

  // merge segments
  mergeSegments(cur_seg, end_idx);
}

void Replay::mergeSegments(int cur_seg, int end_idx) {
  // segments must be merged in sequence.
  std::vector<int> segments_need_merge;
  const int begin_idx = std::max(cur_seg - BACKWARD_SEGS, 0);
  for (int i = begin_idx; i <= end_idx; ++i) {
    if (segments_[i] && segments_[i]->isLoaded()) {
      segments_need_merge.push_back(i);
    } else if (i >= cur_seg) {
      // segment is valid,but still loading. can't skip it to merge the next one.
      // otherwise the stream thread may jump to the next segment.
      break;
    }
  }

  if (segments_need_merge != segments_merged_) {
    qDebug() << "merge segments" << segments_need_merge;
    segments_merged_ = segments_need_merge;

    std::vector<Event *> *new_events = new std::vector<Event *>();
    std::unordered_map<uint32_t, EncodeIdx> *new_eidx = new std::unordered_map<uint32_t, EncodeIdx>[MAX_CAMERAS];
    for (int n : segments_need_merge) {
      auto &log = segments_[n]->log;
      // merge & sort events
      auto middle = new_events->insert(new_events->end(), log->events.begin(), log->events.end());
      std::inplace_merge(new_events->begin(), middle, new_events->end(), Event::lessThan());
      for (CameraType cam_type : ALL_CAMERAS) {
        new_eidx[cam_type].insert(log->eidx[cam_type].begin(), log->eidx[cam_type].end());
      }
    }

    // update events
    auto prev_events = events_;
    auto prev_eidx = eidx_;
    updateEvents([=]() {
      if (route_start_ts_ == 0) {
        // get route start time from initData
        auto it = std::find_if(new_events->begin(), new_events->end(), [=](auto e) { return e->which == cereal::Event::Which::INIT_DATA; });
        if (it != new_events->end()) {
          route_start_ts_ = (*it)->mono_time;
          cur_mono_time_ = route_start_ts_;
        }
      }

      events_ = new_events;
      eidx_ = new_eidx;
    });
    delete prev_events;
    delete[] prev_eidx;
  }

  // free segments out of current semgnt window.
  std::vector<int> removed;
  for (int i = 0; i < segments_.size(); i++) {
    if ((i < begin_idx || i > end_idx) && segments_[i]) {
      segments_[i].reset(nullptr);
      removed.push_back(i);
    }
  }
  if (removed.size() > 0) {
    qDebug() << "remove segments" << removed;
  }
}

void Replay::stream() {
  float last_print = 0;
  cereal::Event::Which cur_which = cereal::Event::Which::INIT_DATA;

  std::unique_lock lk(lock_);

  while (true) {
    stream_cv_.wait(lk, [=]() { return exit_ || (paused_ == false && events_updated_); });
    events_updated_ = false;
    if (exit_) break;

    Event cur_event(cur_which, cur_mono_time_);
    auto eit = std::upper_bound(events_->begin(), events_->end(), &cur_event, Event::lessThan());
    if (eit == events_->end()) {
      qDebug() << "waiting for events...";
      continue;
    }

    qDebug() << "unlogging at" << (int)((cur_mono_time_ - route_start_ts_) * 1e-9);
    uint64_t evt_start_ts = cur_mono_time_;
    uint64_t loop_start_ts = nanos_since_boot();

    for (/**/; !updating_events_ && eit != events_->end(); ++eit) {
      const Event *evt = (*eit);
      cur_which = evt->which;
      cur_mono_time_ = evt->mono_time;

      std::string type;
      KJ_IF_MAYBE(e_, static_cast<capnp::DynamicStruct::Reader>(evt->event).which()) {
        type = e_->getProto().getName();
      }

      if (socks.find(type) != socks.end()) {
        int current_ts = (cur_mono_time_ - route_start_ts_) / 1e9;
        if ((current_ts - last_print) > 5.0) {
          last_print = current_ts;
          qInfo() << "at " << current_ts << "s";
        }
        setCurrentSegment(current_ts / 60);

        // keep time
        long etime = cur_mono_time_ - evt_start_ts;
        long rtime = nanos_since_boot() - loop_start_ts;
        long us_behind = ((etime - rtime) * 1e-3) + 0.5;
        if (us_behind > 0 && us_behind < 1e6) {
          QThread::usleep(us_behind);
        }

        // publish frame
        // TODO: publish all frames
        if (evt->which == cereal::Event::ROAD_CAMERA_STATE) {
          auto it_ = eidx_[RoadCam].find(evt->event.getRoadCameraState().getFrameId());
          if (it_ != eidx_[RoadCam].end()) {
            EncodeIdx &e = it_->second;
            auto &seg = segments_[e.segmentNum]; 
            if (seg && seg->isLoaded()) {
              auto &frm = seg->frames[RoadCam];
              if (vipc_server == nullptr) {
                cl_device_id device_id = cl_get_device_id(CL_DEVICE_TYPE_DEFAULT);
                cl_context context = CL_CHECK_ERR(clCreateContext(NULL, 1, &device_id, NULL, NULL, &err));

                vipc_server = new VisionIpcServer("camerad", device_id, context);
                vipc_server->create_buffers(VisionStreamType::VISION_STREAM_RGB_BACK, UI_BUF_COUNT,
                                            true, frm->width, frm->height);
                vipc_server->start_listener();
              }

              uint8_t *dat = frm->get(e.frameEncodeId);
              if (dat) {
                VisionIpcBufExtra extra = {};
                VisionBuf *buf = vipc_server->get_buffer(VisionStreamType::VISION_STREAM_RGB_BACK);
                memcpy(buf->addr, dat, frm->getRGBSize());
                vipc_server->send(buf, &extra, false);
              }
            }
          }
        }

        // publish msg
        if (sm == nullptr) {
          auto bytes = evt->bytes();
          pm->send(type.c_str(), (capnp::byte *)bytes.begin(), bytes.size());
        } else {
          sm->update_msgs(nanos_since_boot(), {{type, evt->event}});
        }
      }
    }
  }
}
