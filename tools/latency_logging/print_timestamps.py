import argparse
import json
#import plotly.figure_factory as ff
import sys
from collections import defaultdict

from tools.lib.route import Route
from tools.lib.logreader import LogReader
    
DEMO_ROUTE = "9f583b1d93915c31|2022-04-01--08-55-14"

SERVICES = ['camerad', 'modeld', 'plannerd', 'controlsd', 'boardd']

MSGQ_TO_SERVICE = {
  'roadCameraState' : 'camerad',
  'wideRoadCameraState' : 'camerad',
  'modelV2' : 'modeld',
  'lateralPlan' : 'plannerd',
  'longitudinalPlan' : 'plannerd',
  'sendcan' : 'controlsd',
  'controlsState' : 'controlsd'
}

SERVICE_TO_DURATIONS = {
  'camerad' : ['processingTime'],
  'modeld' : ['modelExecutionTime', 'gpuExecutionTime'],
  'plannerd' : ["solverExecutionTime"],
  'controlsd' : [],
  'boardd' : []
}

def get_translation_LUT(logreader):
  translationdict = {}
  # sendcan is the only message where we cannot identify the frame id with certainty, but assuming it has the same as the next ConstrolsState is good enough
  latest_sendcan_monotime = 0
  msgqs = set(MSGQ_TO_SERVICE)
  for msg in logreader:
    if msg.which() in msgqs:
      mono_time = msg.logMonoTime
      if msg.which() == 'modelV2':
        translationdict[mono_time] = msg.modelV2.frameId
      elif msg.which() == 'lateralPlan':
        translationdict[mono_time] = translationdict[msg.lateralPlan.modelMonoTime]
      elif msg.which() == 'sendcan':
        latest_sendcan_monotime = msg.logMonoTime
      elif msg.which() == 'controlsState':
        if latest_sendcan_monotime:
          translationdict[latest_sendcan_monotime] = translationdict[msg.controlsState.lateralPlanMonoTime]
  return translationdict

def read_logs(logreader, translationdict):
  pub_times = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
  start_times = defaultdict(list)
  internal_durations = defaultdict(lambda: defaultdict(lambda: defaultdict(float)))
  frame_mismatches = []
  failed_transl = 0
  def translate(logMono):
    # dont count when logMono is not set yet
    if logMono == 0: 
      return -2
    return -1 if logMono not in translationdict else translationdict[logMono]
  msgqs = set(MSGQ_TO_SERVICE)
  for msg in logreader:
    if msg.which() in msgqs:
      msg_name = msg.which()
      mono_time = msg.logMonoTime
      service = MSGQ_TO_SERVICE[msg_name]
      frame_id = -1
      if msg_name == 'roadCameraState':
        frame_id = msg.roadCameraState.frameId
        start_times[frame_id].append(msg.roadCameraState.timestampSof)
      elif msg_name == 'wideRoadCameraState':
        frame_id = msg.wideRoadCameraState.frameId
        start_times[frame_id].append(msg.wideRoadCameraState.timestampSof)
      elif msg_name == 'modelV2':
        frame_id = msg.modelV2.frameId
        if msg.modelV2.frameIdExtra != frame_id:
          frame_mismatches.append(frame_id)
      elif msg_name == 'lateralPlan':
        frame_id = translate(msg.lateralPlan.modelMonoTime)
      elif msg_name == 'longitudinalPlan':
        frame_id = translate(msg.longitudinalPlan.modelMonoTime)
      elif msg_name == 'sendcan':
        frame_id = translate(mono_time)
      elif msg_name == 'controlsState':
        frame_id = translate(msg.controlsState.lateralPlanMonoTime)
      if frame_id == -1:
        failed_transl += 1
        continue
      pub_times[frame_id][service][msg_name].append(mono_time)
      for duration in SERVICE_TO_DURATIONS[service]:
        internal_durations[frame_id][service][msg_name+"."+duration] = msg.to_dict()[msg_name][duration] 
  return (pub_times, start_times, internal_durations, frame_mismatches, failed_transl)

def get_empty_data(pub_times, start_times):
  for frame_id in pub_times.keys():
    if frame_id not in start_times.keys():
      yield frame_id
      continue
    for service in SERVICES:
      #boardd publication is not logged
      if service not in pub_times[frame_id].keys() and service != 'boardd':
        yield frame_id

def exclude_bad_data(frame_ids, pub_times, start_times):
  for frame_id in frame_ids:
    if frame_id in pub_times:
      del pub_times[frame_id]
    if frame_id in start_times:
      del start_times[frame_id]
     
def build_intervals(pub_times, start_times):
  timestamps = defaultdict(lambda: defaultdict(dict))
  for frame_id, services in pub_times.items():
    timestamps[frame_id]['boardd']['Timestamps'] = []
    for service, msg_names in services.items():
      if service == 'camerad':
        timestamps[frame_id][service]["Start"] = min(start_times[frame_id])
      else:
        prev_service = SERVICES[SERVICES.index(service)-1]
        timestamps[frame_id][service]["Start"] = min(min(times) for times in pub_times[frame_id][prev_service].values())
      timestamps[frame_id][service]["End"] = max(max(msg_names.values()))
      timestamps[frame_id][service]["Timestamps"] = []
      for msg_name, times in msg_names.items():
        timestamps[frame_id][service]["Timestamps"] += [(msg_name+" published", time) for time in times]
  return timestamps

def find_frame_id(time, service, timestamps):
  for frame_id in timestamps.keys():
    if timestamps[frame_id][service]["Start"] <= time <= timestamps[frame_id][service]["End"]:
      return frame_id
  return -1

## ASSUMES THAT AT LEAST ONE CLOUDLOG IS MADE IN CONTROLSD
def insert_cloudlogs(logreader, timestamps):
  t0 = timestamps[min(timestamps.keys())][SERVICES[0]]["Start"] 
  failed_inserts = 0
  latest_controls_frameid = -1
  for msg in logreader:
    if msg.which() == "logMessage":
      try:
        jmsg = json.loads(msg.logMessage)
        if "timestamp" in jmsg['msg']:
          time = int(jmsg['msg']['timestamp']['time'])
          if time < t0:
            continue
          service = jmsg['ctx']['daemon']
          event = jmsg['msg']['timestamp']['event']
          frame_id = latest_controls_frameid if service == "boardd" else find_frame_id(time, service, timestamps)
          if service == 'controlsd':
            latest_controls_frameid = frame_id
          if frame_id > -1:
            timestamps[frame_id][service]["Timestamps"].append((event, time))
          else:
            failed_inserts += 1
      except KeyError:
        pass
  return failed_inserts

def fix_boardd_intervals(timestamps):
  for frame_id in timestamps.keys():
    event_times = [tup[1] for tup in timestamps[frame_id]['boardd']["Timestamps"]]
    if len(event_times) > 0:
      timestamps[frame_id]['boardd']["Start"] = min(event_times)
      timestamps[frame_id]['boardd']["End"] = max(event_times)
    else:
      del timestamps[frame_id]['boardd']

def print_timestamps(timestamps, internal_durations, relative_self):
  t0 = timestamps[min(timestamps.keys())][SERVICES[0]]["Start"] 
  for frame_id, services in timestamps.items():
    print('='*80)
    print("Frame ID:", frame_id)

    print("Timestamps:")
    if relative_self:
      t0 = timestamps[frame_id][SERVICES[0]]["Start"] 
    for service in SERVICES:
      if service not in services:
        continue
      print("  "+service)  
      events = timestamps[frame_id][service]["Timestamps"]
      for event, time in sorted(events, key = lambda x: x[1]):
        print("    "+'%-50s%-50s' %(event, str((time-t0)/1e6)))  

    print("Internal durations:")
    for service, events in internal_durations[frame_id].items():
      print("  "+service)  
      for event, time in dict(events).items():
        print("    "+'%-50s%-50s' %(event, str(time)))  

def graph_timestamps(timestamps, relative_self):
  #TODO
  '''
  t0 = timestamps[min(timestamps.keys())][SERVICES[0]]["Start"] 
  event_bars = []
  service_bars = []
  for frame_id, services in timestamps.items():
    if relative_self:
      t0 = timestamps[frame_id][SERVICES[0]]["Start"] 
    for service in services.keys():
      start = (timestamps[frame_id][service]["Start"]-t0)/1e6
      end = (timestamps[frame_id][service]["End"]-t0)/1e6
      service_bars.append(dict(Task=frame_id, Start=start, Finish=end, Service=service))
      events = timestamps[frame_id][service]["Timestamps"]
      for event, time in events:
        time = (time-t0)/1e6
        event_bars.append(dict(Task=frame_id, Start=time, End=0.1, Event=event))

  fig = ff.create_gantt(service_bars, index_col="Service", group_tasks=True, show_colorbar=True, show_hover_fill=True)
  for bar in fig['data']:
    bar['opacity'] = 0.7
  fig.layout.xaxis.type = 'linear'
  fig.show()
'''

def get_timestamps(lr):
  translationdict = get_translation_LUT(lr)
  pub_times, start_times, internal_durations, frame_mismatches,failed_transl = read_logs(lr, translationdict)
  empty_data = list(set(get_empty_data(pub_times, start_times)))
  exclude_bad_data(set(empty_data+frame_mismatches), pub_times, start_times)
  timestamps = build_intervals(pub_times, start_times)
  failed_inserts = insert_cloudlogs(lr, timestamps)
  fix_boardd_intervals(timestamps)
  return (timestamps, internal_durations, failed_transl, len(frame_mismatches), len(empty_data), failed_inserts)



if __name__ == "__main__":
  parser = argparse.ArgumentParser(description = "A helper to run timestamp print on openpilot routes",
                                   formatter_class = argparse.ArgumentDefaultsHelpFormatter)
 # parser.add_argument("--plot", action = "store_true", help = "If a plot should be generated")
  parser.add_argument("--relative_self", action = "store_true", help = "Print and plot starting a 0 each time")
  parser.add_argument("--demo", action = "store_true", help = "Use the demo route instead of providing one")
  parser.add_argument("route_name", nargs = '?', help = "The route to print")

  if len(sys.argv) == 1:
    parser.print_help()
    sys.exit()
  args = parser.parse_args()

  r = Route(DEMO_ROUTE if args.demo else args.route_name.strip())
  lr = LogReader(r.log_paths()[0], sort_by_time = True)
  timestamps, internal_durations, failed_transl, frame_mismatches, empty_data, failed_inserts = get_timestamps(lr)
  print_timestamps(timestamps, internal_durations, args.relative_self)
 # if args.plot:
  #  graph_timestamps(timestamps, args.relative_self)

  print("Num frames skipped due to failed translations:",failed_transl)
  print("Num frames skipped due to frameId missmatch:",frame_mismatches)
  print("Num frames skipped due to empty data:", empty_data)
  print("Num inserts failed:", failed_inserts)

