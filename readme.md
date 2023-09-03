# ruuvimqtt2influx
## reads ruuvi data send by ruuvi gateways from an mqtt server and write to mqtt and/or Iinfluxdb and/or Grafana Live


### Features

 - use of [paho-c](https://github.com/eclipse/paho.mqtt.c) for MQTT
 - use of libcurl for http,https,ws and wss

### Get started

ruuvimqtt2influx requires a configuration file. By default ./ruuvimqtt2influx.conf is used. You can define another config file using the

```
--configfile=pathOfConfigFile
```

command line parameter.

## command line options or options in the first section of the config file

Long command line options requires to be prefixed with -- while as in the config file the option has to be specified without the prefix. Short command line options can only be used on command line. The descriptions below show the options within the config file, if used on command line, a prefix of -- is required.

```
Usage: ruuvimqtt2influx [OPTION]...
  -h, --help              show this help and exit
  --configfile=           config file name
  -m, --measurement=      Influxdb measurement (Temp)
  -g, --tagname=          Influxdb tag name (Device)
  -s, --server=           influxdb server name or ip
  -o, --port=             influxdb port (8086)
  -b, --db=               Influxdb v1 database name
  -u, --user=             Influxdb v1 user name
  -p, --password=         Influxdb v1 password
  -B, --bucket=           Influxdb v2 bucket
  -O, --org=              Influxdb v2 org
  -T, --token=            Influxdb v2 auth api token
  --influxwritemult=      Influx write multiplicator
  -c, --cache=            #entries for influxdb cache (1000)
  -M, --mqttserver=       mqtt server name or ip
  -C, --mqttprefix=       prefix for mqtt publish
  -R, --mqttport=         ip port for mqtt server (1883)
  -Q, --mqttqos=          default mqtt QOS, can be changed for meter (0)
  -r, --mqttretain=       default mqtt retain, can be changed for meter (0)
  -t, --mqtttopic=        topic for mqtt subscribe (ruuvi)
  -i, --mqttclientid=     mqtt client id
  --ghost=                grafana server url w/o port, e.g. ws://localost or https://localhost
  --gport=                grafana port (3000)
  --gtoken=               authorisation api token for Grafana
  --gpushid=              push id for Grafana
  -a, --map=              id,name - map id to name, can be specified multiple times
  -v, --verbose[=]        increase or set verbose level
  -P, --poll=             poll intervall in seconds
  -y, --syslog            log to syslog insead of stderr
  -Y, --syslogtest        send a testtext to syslog and exit
  -e, --version           show version and exit
  -U, --dryrun[=]         Show what would be written to MQTT/Influx/Grafana
```
### map ruuvi token to name

```
map=F7:66:1C:4E:26:21,Floor1
map=AC:46:A8:04:14:25,Floor2
```
Maps the id of a ruuvi token to a name used for pushing data to Influxdb,Grafana or MQTT.

### InfluxDB - common for version 1 and 2

```
server=ip_or_server_name_of_influxdb_host
port=8086
measurement=energyMeter
tagname=Meter
cache=1000
```

If __server__ is not specified, post to InfluxDB will be disabled at all (if you would like to use MQTT and/or Grafana Live only). Default is http://. To use SSL prefix the hostname by https://, e.g.
```
server=https://myinfluxhost.mydomain.de
port=8086
```

__tagname__ will be the tag used for posting to Influxdb.
__port__ is the IP port number and defaults to 8086
__cache__ is the number of posts that will be cached in case the InfluxDB server is not reachable. This is implemented as a ring buffer. The entries will be posted after the InfluxDB server is reachable again. One post consists of the data for all meters queried at the same time.
__measurement__ sets the default measurement and can be overriden in a meter type or in a meter definition.

### InfluxDB version 1

For version 1, database name, username and password are used for authentication.

```
db=
user=
password=
```

### InfluxDB version 2

Version 2 requires bucket, org and token:

```
bucket=
org=
token=
```
### Grafana Live
Grafana Live is tested with http and ws (Websockets) but should work with https and wss as well. For best performance and lowest overhead, ws:// should be the perferred protocol.
Websocket support, is at the time of writing (07/2023) still beta but seems to work fine. However, current distributions like Fedora 39 or Raspberry (Debian 11 (bullseye)) ships with a shared libcurl that do not support websockets. If you try to use websockets with a shared libcurl and websockets are not supported, emModbus2influx will try fallback to http or https:.
To use websockets, static linking of libcurl can be enabled in Makefile. The Makefile will download a current version of curl and will configure, compile and link this version.
In this is may be required to install additional devel packages required by curl. These are the packages i needed to compile on Debian
```
sudo apt install libmuparser-dev libmuparser2v5 libmodbus-dev libmodbus5 libreadline-dev libpaho-mqtt-dev libpaho-mqtt1.3 libzstd-dev zstd libssl-dev
```
Required parameter
__ghost__ url of the Grafana server without port, e.g. ws://localhost
__gtoken__ token for authentication, at the time of writing (Grafana 10.1.1 OSS) a service account with the role "Admin"is required.
__gpushid__ the push id. Data is pushed to Grafana using
```
ServerAddr:port/api/live/push/gpushid
```
and this will show up in Grafana live as
```
stream/gpushid
```

### MQTT
```
mqttserver=
mqttprefix=ad/house/energy/
mqttport=1883
mqtttopic=ruuvi/#
mqttqos=0
mqttretain=0
mqttclientid=
```

Parameters for MQTT.
__mqttqos__:
- At most once (0)
- At least once (1)
- Exactly once (2)

__mqttretain__:
- no (0)
- yes (1)

If mqttretain is set to 1, mqtt data will only send if data has been changed since last send.

__mqttclientid__:
defaults to ruuvimqtt2influx, needs to be changed if multiple instances of ruuvimqtt2influx are accessing the same mqtt server. This one is used for sending data to the MQTT server (if enabled by specifying mqttprefix). For subscribing, mqttclientid is postfixed by "-SUB".

__mqttclientid__:
Needs to be changed if multiple instances of emModbus2influx are accessing the same mqtt server

__mqttprefix :__
If specified, data will be send back to the MQTT server with the given prefix.

### additional options
```
verbose=0
syslog
poll=300
```

__verbose__: sets the verbisity level
__syslog__: enables messages to syslog instead of stdout
__poll__: sets the interval in seconds for writing to influxdb

### command line only parameters

```
--configfile=
--syslogtest
--version
--dryrun
```
__configfile__: sets the config file to use, default is ./emModbus2influx.conf
**syslogtest**: sends a test message to syslog.
**dryrun**: perform one query of all meters and show what would be posted to InfluxDB / MQTT

