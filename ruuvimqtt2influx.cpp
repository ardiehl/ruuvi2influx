/******************************************************************************
emmbus2influx

Read from mbus energy meters
and send the data to influxdb (1.x or 2.x API) and/or via mqtt
******************************************************************************/
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "argparse.h"
#include <math.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>

#include "log.h"

#include "mqtt_publish.h"

#include "influxdb-post/influxdb-post.h"
#include "global.h"
#include "global.h"
#include <endian.h>

#include "ruuvimqtt.h"
#include "MQTTClient.h"

#define VER "1.04444min Diehl <ad@ardiehl.de> Jul 31,2024, compiled " __DATE__ " " __TIME__
#define ME "ruuvimqtt2influx"
#define CONFFILE "ruuvimqtt2influx.conf"

#define MQTT_CLIENT_ID ME

#define NUM_RECS_TO_BUFFER_ON_FAILURE 1000

char *configFileName;
int dryrun;
int queryIntervalSecs = 60 * 5; // 5 minutes
char *formulaValMeterName;
influx_client_t *iClient;

mqtt_pubT *mClient;
extern int mqttSenderConnectionLost;



#define INFLUX_DEFAULT_MEASUREMENT "Temp"
#define INFLUX_DEFAULT_TAGNAME "Device"
char * influxMeasurement;
char * influxTagName;
int iVerifyPeer = 1;

int influxWriteMult;    // write to influx only on x th query (>=2)
int mqttQOS;
int mqttRetain;
char * mqttprefix;
char * mqttTopic;
#define MQTT_DEF_TOPIC "ruuvi"
char * mqttReceiverClientID;

// Grafana Live
char *ghost;
int gport = 3000;
char *gtoken;
char *gpushid;
influx_client_t *gClient;
int gVerifyPeer = 1;

/* msleep(): Sleep for the requested number of milliseconds. */
int msleep(long msec)
{
    struct timespec ts;
    int res;

    if (msec < 0)
    {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    return res;
}


int syslogTestCallback(argParse_handleT *a, char * arg) {
	VPRINTF(0,"%s : sending testtext via syslog\n\n",ME);
	log_setSyslogTarget(ME);
	VPRINTF(0,"testtext via syslog by %s",ME);
	exit(0);
}


int mapCallback(argParse_handleT *a, char * arg) {
	char *id,*name;

	assert(arg != NULL);
	id = strdup(arg);
	name = strchr(id,',');
	if (!name) {
		EPRINTFN("invalid argument for map (%s), expected id,name");
		exit(1);
	}
	*name = '\0';
	name++;
	//printf("id: '%s', name: '%s'\n",id,name);
	if (! addMapping (id,name)) {
		EPRINTFN("Failed to add mapping for id \"%s\" (%s)",id,name);
		exit(1);
	}
	return 0;
}


int showVersionCallback(argParse_handleT *a, char * arg) {
	MQTTClient_nameValue* MQTTVersionInfo;
	char *MQTTVersion = NULL;
	int i;

	printf("%s %s\n",ME,VER);
	MQTTVersionInfo = MQTTClient_getVersionInfo();
	i = 0;
	while (MQTTVersionInfo[i].name && MQTTVersion == NULL) {
		if (strcasecmp(MQTTVersionInfo[i].name,"Version") == 0) {
			MQTTVersion = (char *)MQTTVersionInfo[i].value;
			break;
		}
		i++;
	}
	if (MQTTVersion)
	printf("paho mqtt-c: %s\n",MQTTVersion);
	exit(2);
}

// to avoid unused error message for --configfile
int dummyCallback(argParse_handleT *a, char * arg) {
	return 0;
}

#define CONFFILEARG "--configfile="

int parseArgs (int argc, char **argv) {
	int res = 0;
	int i;
	char * dbName = NULL;
	char * serverName = NULL;
	char * userName = NULL;
	char * password = NULL;
	char * bucket = NULL;
	char * org = NULL;
	char * token = NULL;
	int syslog = 0;
	int port = 8086;
	int numQueueEntries = NUM_RECS_TO_BUFFER_ON_FAILURE;
	argParse_handleT *a;

	influxMeasurement = strdup(INFLUX_DEFAULT_MEASUREMENT);
	influxTagName = strdup(INFLUX_DEFAULT_TAGNAME);

	AP_START(argopt)
		AP_HELP
		AP_OPT_STRVAL_CB    (0,0,"configfile"    ,NULL                   ,"config file name",&dummyCallback)

		AP_OPT_STRVAL       (1,'m',"measurement"    ,&influxMeasurement    ,"Influxdb measurement")
		AP_OPT_STRVAL       (1,'g',"tagname"        ,&influxTagName        ,"Influxdb tag name")
		AP_OPT_STRVAL       (1,'s',"server"         ,&serverName           ,"influxdb server name or ip")
		AP_OPT_INTVAL       (1,'o',"port"           ,&port                 ,"influxdb port")
		AP_OPT_STRVAL       (1,'b',"db"             ,&dbName               ,"Influxdb v1 database name")
		AP_OPT_STRVAL       (1,'u',"user"           ,&userName             ,"Influxdb v1 user name")
		AP_OPT_STRVAL       (1,'p',"password"       ,&password             ,"Influxdb v1 password")
		AP_OPT_STRVAL       (1,'B',"bucket"         ,&bucket               ,"Influxdb v2 bucket")
		AP_OPT_STRVAL       (1,'O',"org"            ,&org                  ,"Influxdb v2 org")
		AP_OPT_STRVAL       (0,'T',"token"          ,&token                ,"Influxdb v2 auth api token")
		AP_OPT_INTVAL       (0, 0 ,"influxwritemult",&influxWriteMult      ,"Influx write multiplicator")
		AP_OPT_INTVAL       (1,'c',"cache"          ,&numQueueEntries      ,"#entries for influxdb cache")
		AP_OPT_STRVAL       (1,'M',"mqttserver"     ,&mClient->hostname    ,"mqtt server name or ip")
		AP_OPT_STRVAL       (1,'C',"mqttprefix"     ,&mqttprefix           ,"prefix for mqtt publish")
		AP_OPT_INTVAL       (1,'R',"mqttport"       ,&mClient->port        ,"ip port for mqtt server")
		AP_OPT_INTVAL       (1,'Q',"mqttqos"        ,&mqttQOS              ,"default mqtt QOS, can be changed for meter")
		AP_OPT_INTVAL       (1,'r',"mqttretain"     ,&mqttRetain           ,"default mqtt retain, can be changed for meter")
		AP_OPT_STRVAL       (1,'t',"mqtttopic"      ,&mqttTopic            ,"topic for mqtt subscribe")

		AP_OPT_STRVAL       (1,'i',"mqttclientid"   ,&mClient->clientId    ,"mqtt client id")

		AP_OPT_STRVAL       (1,0  ,"ghost"          ,&ghost                ,"grafana server url w/o port, e.g. ws://localost or https://localhost")
		AP_OPT_INTVAL       (1,0  ,"gport"          ,&gport                ,"grafana port")
		AP_OPT_STRVAL       (1,0  ,"gtoken"         ,&gtoken               ,"authorisation api token for Grafana")
		AP_OPT_STRVAL       (1,0  ,"gpushid"        ,&gpushid              ,"push id for Grafana")
		AP_OPT_INTVAL       (1,0  ,"gsslverifypeer" ,&gVerifyPeer          ,"grafana SSL certificate verification (0=off)")

		AP_OPT_STRVAL_CB    (0,'a',"map"            ,NULL                  ,"id,name - map id to name, can be specified multiple times",&mapCallback)
		AP_OPT_INTVALFO     (0,'v',"verbose"        ,&log_verbosity        ,"increase or set verbose level")
		AP_OPT_INTVAL       (1,'P',"poll"           ,&queryIntervalSecs    ,"poll intervall in seconds")
		AP_OPT_INTVALF      (0,'y',"syslog"         ,&syslog               ,"log to syslog insead of stderr")
		AP_OPT_INTVALF_CB   (0,'Y',"syslogtest"     ,NULL                  ,"send a testtext to syslog and exit",&syslogTestCallback)
		AP_OPT_INTVALF_CB   (0,'e',"version"        ,NULL                  ,"show version and exit",&showVersionCallback)
		AP_OPT_INTVALFO     (0,'U',"dryrun"         ,&dryrun               ,"Show what would be written to MQTT/Influx/Grafana")
	AP_END;

	// check if we have a configfile argument
	int len = strlen(CONFFILEARG);

	for (i=1;i<argc;i++) {
		if (strncmp(CONFFILEARG,argv[i],len) == 0) {
			configFileName = strdup(argv[i]+len);
			int fh = open(configFileName,O_RDONLY);
			if (fh < 0) {
				EPRINTFN("unable to open config file '%s'",configFileName);
				exit(1);
			}
			close(fh);
			LOGN(1,"using configfile \"%s\"",configFileName);
			break;
		}
	}

	if (configFileName == NULL) configFileName = strdup(CONFFILE);

	a = argParse_init(argopt, configFileName, NULL,
		"The cache will be used in case the influxdb server is down. In\n" \
        "that case data will be send when the server is reachable again.\n");
	res = argParse (a, argc, argv, 0);
	if (res != 0) {
		argParse_free (a);
		return res;
	}


	if (serverName) {
		LOG(1,"Influx init: serverName: %s, port %d, dbName: %s, userName: %s, password: %s, org: %s, bucket:%s, numQueueEntries %d\n",serverName, port, dbName, userName, password, org, bucket, numQueueEntries);
		iClient = influxdb_post_init (serverName, port, dbName, userName, password, org, bucket, token, numQueueEntries, iVerifyPeer);
	} else {
		free(dbName);
		free(serverName);
		free(userName);
		free(password);
		free(bucket);
		free(org);
		free(token);
	}

	argParse_free (a);

	if (syslog) log_setSyslogTarget(ME);

    return 0;
}


int terminated;


void sigterm_handler(int signum) {
	LOGN(0,"sigterm_handler called, terminating");
	signal(SIGINT, NULL);
	terminated++;
}



void sigusr1_handler(int signum) {
	log_verbosity++;
	LOGN(0,"verbose: %d",log_verbosity);
}

void sigusr2_handler(int signum) {
	if (log_verbosity) log_verbosity--;
	LOGN(0,"verbose: %d",log_verbosity);
}


#define INITIAL_BUFFER_LEN 256

void appendToStr (const char *src, char **dest, int *len, int *bufsize) {
	int srclen;

	if (src == NULL) return;
	if (*src == 0) return;

	srclen = strlen(src);
	if (*len + srclen + 1 > *bufsize) {
		*bufsize *= 2;
		//printf("Realloc to %d, len=%d, srclen: %d %x %x %s\n",*bufsize,*len,srclen,dest,*dest,*dest);
		*dest = (char *)realloc(*dest,*bufsize);
		if (*dest == NULL) { EPRINTF("Out of memory in appendToStr"); exit(1); };
	}
	strcat(*dest,src);
	*len += srclen;
}


#define APPEND(SRC) appendToStr(SRC,&buf,&buflen,&bufsize)
#define APPENDFLOAT(name,value,dec) sprintf(tempStr,"%s\"" #name "\"" ":%1." #dec "f",first?"":", ",value); APPEND(tempStr)
#define APPENDINT(name,value) sprintf(tempStr,"%s\"" #name "\"" ":%d",first?"":", ",value); APPEND(tempStr)
int mqttSendData (dataRead_t * dr,int dryrun) {
	int bufsize = INITIAL_BUFFER_LEN;
	char *buf;
	int buflen = 0;
	int rc = 0;
	char tempStr[255];
	char *name,*s;
	int first = 1;

#if 0
	double deltaTemperature = dr->dataLastSent.temperature-dr->dataCurr.temperature;
    double deltaHumidity = dr->dataLastSent.humidity-dr->dataCurr.humidity;
    int deltaPressure = dr->dataLastSent.pressure-dr->dataCurr.pressure;
    int deltaBatteryVoltage = dr->dataLastSent.batteryVoltage-dr->dataCurr.batteryVoltage;
    // we do not need the full resolution
    if (fabs(deltaTemperature) < 0.09 && fabs(deltaHumidity) < 0.09 && abs(deltaPressure) < 10 && abs (deltaBatteryVoltage < 50)) {
		dr->updated = 0;
		return 0;
    }
#endif // 0

	buf = (char *)malloc(bufsize);
	if (buf == NULL) return -1;
	*buf=0;


	APPEND("{\"name\":\"");
	if (influxMeasurement) {
			APPEND(influxMeasurement); APPEND(".");
	}
	APPEND(dr->name);
	APPEND("\", ");
	APPENDFLOAT(Temp,dr->dataCurr.temperature,2); first--;
    APPENDFLOAT(Humidity,dr->dataCurr.humidity,1);
    APPENDFLOAT(BattVoltage,(double)dr->dataCurr.batteryVoltage/1000,2);
    APPENDINT(Pressure,dr->dataCurr.pressure);
    dr->dataLastSent = dr->dataCurr;
    APPEND("}");

    if (dr->name) {
        name = strdup(dr->name);
    } else {
        // use mac address
        sprintf(tempStr,"%012lx",dr->mac);
        s = &tempStr[0];
        while (*s) {
            *s = toupper(*s);
            s++;
        }
        name = strdup(tempStr);
    }

	if (dryrun) {
		printf("Dryrun MQTT - %s = %s\n",name,buf);
	} else {
		mClient->topicPrefix = mqttprefix;
		rc = mqtt_pub_strF (mClient,name, 250, mqttQOS,mqttRetain, buf);
		mClient->topicPrefix = NULL;
		if (rc != MQTTCLIENT_SUCCESS && rc != MQTT_RECONNECTED) {
			LOGN(0,"mqtt publish failed with rc: %d",rc);
			free(buf);
			free(name);
			return rc;
		}
		//printf("mqtt_pub_strF: rc: %d\n",rc);
	}

	dr->dataLastSent = dr->dataCurr;

	free(buf);
	free(name);
	return rc;
}



int influxAppendData (influx_client_t* c, dataRead_t * data, uint64_t timestamp) {

	if (data->dataInflux.temperature < -900) {
		//EPRINTFN("influxAppendData: internal program error, would write -999 as temp");
		// can happen if the mqqt sender was disconnected, will be ok again after a reconnect
		return 0;
	}
	influxdb_format_line(c,
                INFLUX_MEAS(influxMeasurement),
                INFLUX_TAG(influxTagName, data->name),
				INFLUX_F_FLT("Temp",data->dataInflux.temperature,1),
				INFLUX_F_FLT("BattVoltage",(float)data->dataInflux.batteryVoltage/1000,1),
				INFLUX_F_FLT("Humidity",data->dataInflux.humidity,1),
				INFLUX_TS(timestamp),
				INFLUX_END);
    data->dataInflux.temperature = 0;
    data->dataInflux.humidity = 0;
	return 0;
}


void GrafanaWriteData (influx_client_t *c) {
	if (!c) return;

	influxdb_post_freeBuffer(c);
	dataRead_t *dataRead = mqttDataRead;
	char fieldName[255];
	int rc;
	int numLines = 0;

	influxdb_post_freeBuffer(c);
	rc = influxdb_format_line(c,INFLUX_MEAS(influxMeasurement),INFLUX_END);
	if (rc < 0) { EPRINTFN("influxdb_format_line failed, rc:%d, INFLUX_MEAS",rc); exit(1); }

	dataRead = mqttDataRead;
	while(dataRead) {
		snprintf(fieldName,sizeof(fieldName),"%s.temp",dataRead->name);	rc = influxdb_format_line(c,INFLUX_F_FLT(fieldName,dataRead->dataInflux.temperature,1),INFLUX_END);
		if (rc < 0) { EPRINTFN("influxdb_format_line failed, rc:%d, %s",rc,fieldName); exit(1); }
		snprintf(fieldName,sizeof(fieldName), "%s.U",dataRead->name); rc = influxdb_format_line(c,INFLUX_F_FLT(fieldName,(float)dataRead->dataInflux.batteryVoltage/1000,2),INFLUX_END);
		if (rc < 0) { EPRINTFN("influxdb_format_line failed, rc:%d, %s",rc,fieldName); exit(1); }
		snprintf(fieldName,sizeof(fieldName), "%s.Humidity",dataRead->name); rc = influxdb_format_line(c,INFLUX_F_FLT(fieldName,dataRead->dataInflux.humidity,1),INFLUX_END);
		if (rc < 0) { EPRINTFN("influxdb_format_line failed, rc:%d, %s",rc,fieldName); exit(1); }
		numLines += 3;
		dataRead = dataRead->next;
	}
	rc = influxdb_format_line(c,INFLUX_TSNOW,INFLUX_END);
	if (rc < 0) { EPRINTFN("influxdb_format_line failed, rc:%d, TS_NOW",rc); exit(1); }

	if (dryrun) {
		if (c->influxBufLen) {
			printf("\nDryrun: would send to grafana:\n%s\n",c->influxBuf);
			influxdb_post_freeBuffer(c);
		} else printf("nothing to be posted to Grafana\n");
	} else {
		if (c->influxBufLen) {
			rc = influxdb_post_http_line(c);
			if (rc != 0) {
				EPRINTFN("Error: influxdb_post_http_line to grafana failed with rc %d",rc);
			} else {
				VPRINTFN(1,"%d lines posted to grafana",numLines);
			}
		} else {
			VPRINTFN(2,"nothing to send to grafana");
		}
	}
}


void traceCallback(enum MQTTCLIENT_TRACE_LEVELS level, char *message) {
	printf(message); printf("\n");
}

#define NANO_PER_SEC 1000000000.0

int main(int argc, char *argv[]) {
	int rc;
	int64_t influxTimestamp;
	time_t nextSendTime,now;
	int isFirstQuery = 1;
	dataRead_t *dr;

	mqttTopic  = strdup(MQTT_DEF_TOPIC);

	mClient = mqtt_pub_init (NULL, 0, NULL, NULL);

	if (parseArgs(argc,argv) != 0) exit(1);

	if (!mClient->clientId) mClient->clientId = strdup(MQTT_CLIENT_ID);

	mqttReceiverClientID = (char *)malloc(strlen(mClient->clientId)+1+4);  // -SUB
	strcpy(mqttReceiverClientID,mClient->clientId);
	strcat(mqttReceiverClientID,"-SUB");

	if (sizeof(time_t) <= 4) {
		LOGN(0,"Warning: TimeT is less than 64 bit, this may fail after year 2038, recompile with newer kernel and glibc to avoid this");
	}

	if (!iClient) LOGN(0,"no influxdb host specified, influx sender disabled");

	if (!mClient->hostname) {
		mqtt_pub_free(mClient);
		mClient = NULL;
		LOGN(0,"no mqtt host specified, mqtt sender disabled");
		if (!iClient) {
			EPRINTFN("No mqtt host and no influxdb host specified, specify one or both");
			exit(1);
		}
	} else {
        LOGN(0,"connecting to mqqt server %s",mClient->hostname);
		rc = mqtt_pub_connect (mClient);
		if (rc != 0) LOGN(0,"mqtt_pub_connect returned %d, will retry later",rc);
	}

	if (ghost && gtoken && gpushid) {
		gClient = influxdb_post_init_grafana (ghost, gport, gpushid, gtoken, gVerifyPeer);
	} else
		LOGN(0,"no grafana host,token or pushid specified, grafana sender disabled");

	rc = mqttReceiverInit (mClient->hostname, mClient->port, mqttTopic, mqttReceiverClientID);
	if (!rc) {
		EPRINTFN("failed to init mqttReceiver for %s:%d, topic: %s",mClient->hostname,mClient->port,mqttTopic);
		exit(1);
	}

	LOGN(0,"mainloop started (%s %s)",ME,VER);


	// term handler for ^c and SIGTERM send by systemd
	signal(SIGTERM, sigterm_handler);
	signal(SIGINT, sigterm_handler);

	signal(SIGUSR1, sigusr2_handler);	// used for verbose level inc/dec via kill command
	signal(SIGUSR2, sigusr1_handler);

	int loopCount = 0;
	now = time(NULL);
	nextSendTime = now + queryIntervalSecs;
	if (verbose || dryrun) {
		LOG (0,"Influx next write time: %s",ctime(&nextSendTime));
		LOGN(0,"                   now: %s (interval: %d)",strtok(ctime(&now),"\n"),queryIntervalSecs);
	}

	while (!terminated) {

		loopCount++;
		//if (dryrun) printf("- %d -----------------------------------------------------------------------\n",loopCount);
		mqtt_pub_yield (mClient); 			// for mqtt ping, sleeps for 100ms if no mqqt specified
		if (gClient) influxdb_post_http(gClient);	// for websocket ping


		if (iClient) {		// influx
			if (time(0) >= nextSendTime) {
				influxdb_post_freeBuffer(iClient);
				influxTimestamp = influxdb_getTimestamp();
				dataRead_t *dataRead = mqttDataRead;
				while(dataRead) {
					influxAppendData (iClient, dataRead, influxTimestamp);
					dataRead = dataRead->next;
				}
				if (dryrun) {
					if (iClient->influxBufLen) printf("\nDryrun: would send to influxdb:\n%s\n",iClient->influxBuf);
					else printf("Dryrun: nothing to be send to influxdb\n");
					influxdb_post_freeBuffer(iClient);
                    dryrun--;
                    if (!dryrun) terminated++;
				} else {
					//printf("Posting to influxdb, len:%ld\n",iClient->influxBufLen);
					if (iClient->influxBufLen) {
						rc = influxdb_post_http_line(iClient);
						influxdb_post_freeBuffer(iClient);
						if (rc != 0) {
							LOGN(0,"Error: influxdb_post_http_line failed with rc %d",rc);
						}
					}
				}
				nextSendTime = time(NULL) + queryIntervalSecs;
			}
		} else
            if (dryrun) {
                dryrun--;
                if (!dryrun) terminated++;
            }

		int numChanged = 0;
		if ((mClient && mqttprefix) || gClient) {		// mqtt
			dr = mqttDataRead;
			while(dr) {
				if (dr->updated) {
					//if (dryrun && !dryRunMsg) printf("Dryrun: would send to mqtt:\n");
					dr->updated = 0;
					numChanged++;
					if (mClient && mqttprefix) mqttSendData (dr,dryrun);
				}
				dr = dr->next;
			}
			if (numChanged) GrafanaWriteData(gClient);
		}

		msleep(200);
		if (gClient) influxdb_post_http(iClient);	// for websocket ping


		if (isFirstQuery) isFirstQuery--;

		if (mqttSenderConnectionLost) {
		    if (!mqttReceiver_isConnected()) {
				mqttReceiverDone(mqttTopic);
				msleep(1500);
				rc = mqttReceiverInit (mClient->hostname, mClient->port, mqttTopic, ME "-SUB");
				if (rc) {
					mqttSenderConnectionLost = 0;
					LOGN(0,"mqtt receiver reconnected");
				} else {
					rc = 150;		// wait 15 seconds before the next connect attempt
					while (rc && !terminated) {
						rc--;
						msleep(100);
					}
				}
			} else {
			  EPRINTFN("Got disconnect callback but client is connected, will not perform reconnect");
			  mqttSenderConnectionLost = 0;
			}
		}


	}

	mqttReceiverDone(mqttTopic);

	if (mClient) mqtt_pub_free(mClient);
	influxdb_post_free(iClient);

    free(configFileName);
	free(mqttprefix);

	free(influxMeasurement);
	free(influxTagName);
	free(mqttTopic);

	LOGN(0,"terminated");

	return 0;
}
