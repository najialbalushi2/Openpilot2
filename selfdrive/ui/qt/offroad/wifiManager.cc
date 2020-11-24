#include <algorithm>
#include <set>
#include<iostream>
#include "wifiManager.hpp"

/**
 * We are using a NetworkManager DBUS API : https://developer.gnome.org/NetworkManager/1.26/spec.html
 * */
 
QString nm_path                = "/org/freedesktop/NetworkManager";
QString nm_settings_path       = "/org/freedesktop/NetworkManager/Settings";

QString nm_iface               = "org.freedesktop.NetworkManager";
QString props_iface            = "org.freedesktop.DBus.Properties";
QString nm_settings_iface      = "org.freedesktop.NetworkManager.Settings";
QString nm_settings_conn_iface = "org.freedesktop.NetworkManager.Settings.Connection";
QString device_iface           = "org.freedesktop.NetworkManager.Device";
QString wireless_device_iface  = "org.freedesktop.NetworkManager.Device.Wireless";
QString ap_iface               = "org.freedesktop.NetworkManager.AccessPoint";
QString connection_iface       = "org.freedesktop.NetworkManager.Connection.Active";

QString nm_service             = "org.freedesktop.NetworkManager";


template <typename T>
T get_response(QDBusMessage response){
  QVariant first =  response.arguments().at(0);
  QDBusVariant dbvFirst = first.value<QDBusVariant>();
  QVariant vFirst = dbvFirst.variant();
  return vFirst.value<T>();
}

bool compare_by_strength(const Network &a, const Network &b){
  if (a.connected == ConnectedType::CONNECTED) return true;
  if (b.connected == ConnectedType::CONNECTED) return false;
  return a.strength > b.strength;
}

WifiManager::WifiManager(){
  qDBusRegisterMetaType<Connection>();

  adapter = get_adapter();
  has_adapter = adapter != "";
  if(has_adapter){
    QDBusInterface nm(nm_service, adapter, device_iface, bus);
    bus.connect(nm_service, adapter, device_iface, "StateChanged", this, SLOT(change(unsigned int, unsigned int, unsigned int)));
    
    QDBusInterface device_props(nm_service, adapter, props_iface, bus);
    QDBusMessage response = device_props.call("Get", device_iface, "State");
    raw_adapter_state = get_response<uint>(response);
    change(raw_adapter_state, 0, 0);
    qDebug()<<"Device state"<<raw_adapter_state;
  }else{
    qDebug()<<"No wifi device connected";
  }
}

void WifiManager::refreshNetworks(){
  if (!has_adapter) return;

  bus = QDBusConnection::systemBus();
  seen_networks.clear();
  seen_ssids.clear();

  for (Network &network : get_networks()){
    if(seen_ssids.count(network.ssid)){
      continue;
    }
    seen_ssids.push_back(network.ssid);
    seen_networks.push_back(network);
  }
  qDebug()<<"Device state"<<raw_adapter_state;
  // for(auto p:get_active_connections()){
  //   qDebug()<<"We have "<<p.path();
  // }
}

QList<Network> WifiManager::get_networks(){
  QList<Network> r;
  QDBusInterface nm(nm_service, adapter, wireless_device_iface, bus);
  QDBusMessage response = nm.call("GetAllAccessPoints");
  QVariant first =  response.arguments().at(0);

  QString active_ap = get_active_ap();

  const QDBusArgument &args = first.value<QDBusArgument>();
  args.beginArray();
  while (!args.atEnd()) {
    QDBusObjectPath path;
    args >> path;

    QByteArray ssid = get_property(path.path(), "Ssid");
    unsigned int strength = get_ap_strength(path.path());
    SecurityType security = getSecurityType(path.path());
    ConnectedType ctype;
    if(path.path()!=active_ap){
      ctype = ConnectedType::DISCONNECTED;
    }else{
      //TODO, determine if connecting
      ctype = ConnectedType::CONNECTED;
    }
    Network network = {path.path(), ssid, strength, ctype, security};

    if (ssid.length()){
      r.push_back(network);
    }
  }
  args.endArray();

  std::sort(r.begin(), r.end(), compare_by_strength);
  return r;
}

SecurityType WifiManager::getSecurityType(QString path){
  int sflag = get_property(path, "Flags").toInt();
  int wpaflag = get_property(path, "WpaFlags").toInt();
  int rsnflag = get_property(path, "RsnFlags").toInt();
  int wpa_props = wpaflag | rsnflag;

  if(sflag == 0){
    return SecurityType::OPEN;
  } else if((sflag & 0x1) && (wpa_props & (0x333) && !(wpa_props & 0x200))) {
    return SecurityType::WPA;
  } else {
    // qDebug() << "Cannot determine security type for " << get_property(path, "Ssid") << " with flags";
    // qDebug() << "flag    " << sflag;
    // qDebug() << "WpaFlag " << wpaflag;
    // qDebug() << "RsnFlag " << rsnflag;
    return SecurityType::UNSUPPORTED;
  }
}

void WifiManager::connect(Network n){
  return connect(n, "", "");
}

void WifiManager::connect(Network n, QString password){
  return connect(n, "", password);
}

void WifiManager::connect(Network n, QString username, QString password){
  QString active_ap = get_active_ap();
  deactivate_connections(get_property(active_ap, "Ssid")); //Disconnect from any connected networks 
  clear_connections(n.ssid); //Clear all connections that may already exist to the network we are connecting
  qDebug() << "Connecting to"<< n.ssid << "with username, password =" << username << "," <<password;
  connect(n.ssid, username, password, n.security_type);
}

void WifiManager::connect(QByteArray ssid, QString username, QString password, SecurityType security_type){
  Connection connection;
  connection["connection"]["type"] = "802-11-wireless";
  connection["connection"]["uuid"] = QUuid::createUuid().toString().remove('{').remove('}');

  connection["connection"]["id"] = "OpenPilot connection "+QString::fromStdString(ssid.toStdString());

  connection["802-11-wireless"]["ssid"] = ssid;
  connection["802-11-wireless"]["mode"] = "infrastructure";

  if(security_type == SecurityType::WPA){
    connection["802-11-wireless-security"]["key-mgmt"] = "wpa-psk";
    connection["802-11-wireless-security"]["auth-alg"] = "open";
    connection["802-11-wireless-security"]["psk"] = password;
  }

  connection["ipv4"]["method"] = "auto";
  connection["ipv6"]["method"] = "ignore";

  QDBusInterface nm_settings(nm_service, nm_settings_path, nm_settings_iface, bus);
  QDBusReply<QDBusObjectPath> result = nm_settings.call("AddConnection", QVariant::fromValue(connection));
  if (!result.isValid()) {
    qDebug() << result.error().name() << result.error().message();
  } else {
    qDebug() << result.value().path();
  }
}

void WifiManager::deactivate_connections(QString ssid){
  for(QDBusObjectPath active_connection_raw:get_active_connections()){
    QString active_connection = active_connection_raw.path();
    QDBusInterface nm(nm_service, active_connection, props_iface, bus);
    uint state = get_response<uint>(nm.call("Get", connection_iface, "State"));
    uint stateFlags = get_response<uint>(nm.call("Get", connection_iface, "StateFlags"));
    qDebug() << active_connection;
    qDebug() << state;
    qDebug() << stateFlags;
    QDBusObjectPath pth = get_response<QDBusObjectPath>(nm.call("Get", connection_iface, "SpecificObject"));
    qDebug() << pth.path();//This is an accessPoint!!! Get property should work
    QString Ssid = get_property(pth.path(), "Ssid");
    qDebug() << Ssid << ssid;
    if(Ssid == ssid){
      QDBusInterface nm2(nm_service, nm_path, nm_iface, bus);
      qDebug() << nm2.call("DeactivateConnection", QVariant::fromValue(active_connection_raw));
    }    
    qDebug()<<"";
  }
}

QVector<QDBusObjectPath> WifiManager::get_active_connections(){
  QDBusInterface nm(nm_service, nm_path, props_iface, bus);
  QDBusMessage response = nm.call("Get", nm_iface, "ActiveConnections");
  QDBusArgument arr = get_response<QDBusArgument>(response);
  QDBusObjectPath path;
  QVector<QDBusObjectPath> conns;
  arr.beginArray();
  while (!arr.atEnd()){
    arr >> path;
    // QString conn_active_path = path.path();
    conns.push_back(path);
  }
  arr.endArray();
  return conns;
}

void WifiManager::clear_connections(QString ssid){
  QDBusInterface nm(nm_service, nm_settings_path, nm_settings_iface, bus);
  QDBusMessage response = nm.call("ListConnections");
  QVariant first =  response.arguments().at(0);
  const QDBusArgument &args = first.value<QDBusArgument>();
  args.beginArray();
  while (!args.atEnd()) {
    QDBusObjectPath path;
    args >> path;
    QDBusInterface nm2(nm_service, path.path(), nm_settings_conn_iface, bus);
    QDBusMessage response = nm2.call("GetSettings");

    const QDBusArgument &dbusArg = response.arguments().at(0).value<QDBusArgument>();

    QMap<QString,QMap<QString,QVariant> > map;
    dbusArg >> map;
    for(QString outer_key : map.keys()) {
      QMap<QString,QVariant> innerMap = map.value(outer_key);
      for(QString inner_key : innerMap.keys()) {
        if(inner_key == "ssid"){
          QString value = innerMap.value(inner_key).value<QString>();
          if(value == ssid){
            // qDebug()<<"Deleting "<<value;
            nm2.call("Delete");
          }
        }
      }
    }
  }
}

void WifiManager::request_scan(){
  if (!has_adapter) return;

  QDBusInterface nm(nm_service, adapter, wireless_device_iface, bus);
  QDBusMessage response = nm.call("RequestScan",  QVariantMap());
}

uint WifiManager::get_wifi_device_state(){
  QDBusInterface device_props(nm_service, adapter, props_iface, bus);
  QDBusMessage response = device_props.call("Get", device_iface, "State");
  uint resp = get_response<uint>(response);
  return resp;
}

QString WifiManager::get_active_ap(){
  QDBusInterface device_props(nm_service, adapter, props_iface, bus);
  QDBusMessage response = device_props.call("Get", wireless_device_iface, "ActiveAccessPoint");
  QDBusObjectPath r = get_response<QDBusObjectPath>(response);
  return r.path();
}

QByteArray WifiManager::get_property(QString network_path ,QString property){
  QDBusInterface device_props(nm_service, network_path, props_iface, bus);
  QDBusMessage response = device_props.call("Get", ap_iface, property);
  return get_response<QByteArray>(response);
}

unsigned int WifiManager::get_ap_strength(QString network_path){
  QDBusInterface device_props(nm_service, network_path, props_iface, bus);
  QDBusMessage response = device_props.call("Get", ap_iface, "Strength");
  return get_response<unsigned int>(response);
}

QString WifiManager::get_adapter(){

  QDBusInterface nm(nm_service, nm_path, nm_iface, bus);
  QDBusMessage response = nm.call("GetDevices");
  QVariant first =  response.arguments().at(0);

  QString adapter_path = "";

  const QDBusArgument &args = first.value<QDBusArgument>();
  args.beginArray();
  while (!args.atEnd()) {
    QDBusObjectPath path;
    args >> path;

    // Get device type
    QDBusInterface device_props(nm_service, path.path(), props_iface, bus);
    QDBusMessage response = device_props.call("Get", device_iface, "DeviceType");
    uint device_type = get_response<uint>(response);

    if (device_type == 2) { // Wireless
      adapter_path = path.path();
      break;
    }
  }
  args.endArray();

  return adapter_path;
}

void WifiManager::change(unsigned int a,unsigned int b,unsigned int c){
  qDebug()<<"CHANGE!"<<b<<"-->"<<a<<" reason:"<<c;
  raw_adapter_state = a;
  if(a==60 && c==8){
    QDBusInterface nm(nm_service, get_active_connections()[0].path(), props_iface, bus);
    wrongPassword(get_property(get_response<QDBusObjectPath>(nm.call("Get", connection_iface, "SpecificObject")).path(), "Ssid"));
  }
}
