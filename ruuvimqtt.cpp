#include "ruuvimqtt.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "log.h"
#include "MQTTClient.h"
#include "cJSON.h"
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

nameMappings_t *nameMappings;
unmappedDevices_t *unmappedDevices;
dataRead_t *mqttDataRead;
MQTTClient client;

int mqttReceiver_isConnected() {
  return MQTTClient_isConnected(client);
}

nameMappings_t * findName (const char * name) {
	if (!nameMappings) return NULL;
	nameMappings_t *nm = nameMappings;
	while (nm) {
		if (strcmp(nm->name,name) == 0) return nm;
		nm = nm->next;
	}
	return NULL;
}

nameMappings_t * findMac (int64_t mac) {
	if (!nameMappings) return NULL;
	nameMappings_t *nm = nameMappings;
	while (nm) {
		if (nm->mac == mac) return nm;
		nm = nm->next;
	}
	return NULL;
}

int addMapping (const char *tokenMac, const char *name) {
    char *macStr,*src,*dst;
    int64_t mac;

	assert(tokenMac != NULL);
	assert(name != NULL);
	assert(strlen(name) > 0);

	nameMappings_t *nm = findName(name);
	if (nm) {
		EPRINTFN("Duplicate name for token mac %s, name \"%s\" already defined for token with mac %012lx",tokenMac,name,nm->mac);
		return 0;
	}

	// remove : from tokenMac
	macStr = (char *)calloc(1,strlen(tokenMac)+1);
	src = (char *)tokenMac; dst = (char *)macStr;
	while (*src) {
        if (*src != ':') { *dst = *src; dst++; }
        src++;
    }
    mac = hex2int(macStr,strlen(macStr),false);
	nm = findMac (mac);
	if (nm) {
		EPRINTFN("Name for token mac %012lx already defined (%s)",mac,name);
		return 0;
	}
	nm = nameMappings;
	if (!nm) {
		nameMappings = (nameMappings_t *)calloc(1,sizeof(nameMappings_t));
		nm = nameMappings;
	} else {
		while (nm->next) nm = nm->next;
		nm->next = (nameMappings_t *)calloc(1,sizeof(nameMappings_t));
		nm = nm->next;
	}
	nm->name = strdup(name);
	nm->mac = mac;
	LOGN(1,"added mapping %012lx = \"%s\"",mac,name);
	free(macStr);
	return 1;
}


unmappedDevices_t * findUnknownMac (int64_t mac) {
	if (!unmappedDevices) return NULL;
	unmappedDevices_t *nm = unmappedDevices;
	while (nm) {
		if (nm->mac == mac) return nm;
		nm = nm->next;
	}
	return NULL;
}


int addUnknownDevice (int64_t mac) {

	unmappedDevices_t *nm = findUnknownMac (mac);
	if (nm) return 0;

	nm = unmappedDevices;
	if (!nm) {
		unmappedDevices = (unmappedDevices_t *)calloc(1,sizeof(unmappedDevices_t));
		nm = unmappedDevices;
	} else {
		while (nm->next) nm = nm->next;
		nm->next = (unmappedDevices_t *)calloc(1,sizeof(unmappedDevices_t));
		nm = nm->next;
	}
	nm->mac = mac;
	LOGN(0,"added unknown mapping %012lx",mac);
	return 1;
}

int hexnib2int(char c) {
    c = toupper(c);
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

uint64_t int_pow(uint64_t base, uint64_t exp)
{
    uint64_t result = 1;
    while (exp)
    {
        if (exp % 2)
           result *= base;
        exp /= 2;
        base *= base;
    }
    return result;
}


int64_t hex2int (const char *src, int nibbles, int isSigned) {
    int64_t res = 0;
    int digit = 0;
    char *s = (char *)src + nibbles -1;
    int nib = nibbles;
    while (nib) {
        nib--;
        res |= (int64_t)hexnib2int(*s) << (4*digit);
        s--; digit++;
    }
    if (isSigned) {
        int64_t mask = (int64_t) 1 << (3+((nibbles-1)*4));

        if (res & mask) {
            uint64_t minVal = (int_pow(2,(nibbles*4)-1) -1) * -1;
            res &= ~mask;
            res = minVal + res;
        }
    }
    return res;
}


int64_t getIntFromHex(char **work, int *remaining, int bytes, int isSigned) {
    if (*remaining < (bytes*2)) return 0;
    int64_t res = hex2int(*work,bytes*2,isSigned);
    *work += bytes*2;
    *remaining -= bytes*2;
    return res;
}

// https://github.com/ruuvi/ruuvi-sensor-protocols/blob/master/dataformat_05.md
/*
  "rssi": -60,
  "rssi": -61,
  "aoa": [],
  "gwts": "1667480128",
  "ts": "1667480128",
  "data": "0201061BFF9904050F0853DAC3C80010FFE00418B196940D3EF0661B4D4621",
           0201061BFF9904 05 0594 1A5BC7B1FFE0001C043867366F2497ED4DFAE75678
  "gwts": "1667480130",
  "ts": "1667480130",
  "data": "020106 1BFF9904 050F0A53E6C3CC0010FFE40418B196940D3FF0661B4D4621"
*/

#define SKIP(BYTES) remaining-=BYTES*2; work+=BYTES*2
int processRuuviData(char * data, int rssi) {
    int len;
    char *work = data;
    int remaining = strlen(data);
    int i,dataFormat;
    double temperature, humidity, deltaTemperature, deltaHumidity;
    int pressure,deltaPressure,batteryVoltage,txpower,movementCounter,measurementSequence,deltaRssi;
    int64_t macAddress;
    dataRead_t *dr;
    nameMappings_t *nm;

    if (remaining % 2 != 0) {
        EPRINTFN("%s: data length is %d, even length expected, data: \"%s\"",__PRETTY_FUNCTION__,remaining,data);
        return false;
    }

    // skip Type Flags and Flag value
    len = getIntFromHex(&work, &remaining, 1, false);  // length byte
    SKIP(len);

    if (remaining < 1) {
        EPRINTFN("%s: no more data after first header, data: \"%s\"",__PRETTY_FUNCTION__,data);
        return false;
    }
    len = getIntFromHex(&work, &remaining, 1, false);  // payload length byte
    if (len != 27) {
        EPRINTFN("%s: got payload length of %d bytes, expected 27, data: \"%s\"",__PRETTY_FUNCTION__,len,data);
        return false;
    }

    i = getIntFromHex(&work, &remaining, 1, false);  // FF : Type Manufacturer Specific data
    if (i != 0xff) {
        EPRINTFN("%s: expected 0xff (Manufacturer Specific data) but got 0x%02x, data: \"%s\"",__PRETTY_FUNCTION__,i,data);
        return false;
    }

    i = getIntFromHex(&work, &remaining, 2, false);  // 9904 : Manufacturer: Ruuvi Innovations (Least Significant Byte first)
    if (i != 0x9904) {
        EPRINTFN("%s: expected 0x9904 (Manufacturer: Ruuvi Innovations) but got 0x%04x, data: \"%s\"",__PRETTY_FUNCTION__,i,data);
        return false;
    }

    dataFormat = getIntFromHex(&work, &remaining, 1, false);

    switch (dataFormat) {
        case 5:
            temperature = (double)getIntFromHex(&work,&remaining,2,true) * 0.005;
            humidity = (double)getIntFromHex(&work,&remaining,2,false) * 0.0025;
            pressure = getIntFromHex(&work,&remaining,2,false);
            if (pressure == 0xffff) pressure = 0; else pressure += 50000;
            SKIP(2);    // Acceleration-Y
            SKIP(2);    // Acceleration-X
            SKIP(2);    // Acceleration-Z
            i = getIntFromHex(&work,&remaining,2,false);
            batteryVoltage = (i >> 5) + 1600;
            if ((i & 0x01f) == 0b11111) txpower = 0; else txpower = -40 + ((i  & 0x01f) * 2);
            movementCounter = getIntFromHex(&work,&remaining,1,false);
            if (movementCounter == 0xff) movementCounter = 0;
            measurementSequence = getIntFromHex(&work,&remaining,2,false);
            macAddress = getIntFromHex(&work,&remaining,6,false);
            break;
        default:
            EPRINTFN("%s: RUUVI data format %d not supported");
            return false;
    }

    nm = findMac (macAddress);
    if (!nm) addUnknownDevice (macAddress);
    // add or update in mqttDataRead
    dr = mqttDataRead;
    if (! dr) {
        dr = (dataRead_t *)calloc(1,sizeof(dataRead_t));
        mqttDataRead = dr;
        dr->mac = macAddress;
        if (nm) dr->name = nm->name;
        dr->dataInflux.temperature = -999;
        dr->dataInflux.humidity = -999;
    } else {
        while (dr->mac != macAddress) {
            if (dr->next) dr = dr->next;
            else {
                dr->next = (dataRead_t *)calloc(1,sizeof(dataRead_t));
                dr->next->mac = macAddress;
                dr = dr->next;
                if (nm) dr->name = nm->name;
                dr->dataInflux.temperature = -999;
                dr->dataInflux.humidity = -999;
            }
        }
    }
    // set values in dr
    free (dr->rawData); dr->rawData = strdup(data);
    deltaTemperature = temperature-dr->dataCurr.temperature;
    dr->dataCurr.temperature = temperature;
    deltaHumidity = humidity-dr->dataCurr.humidity;
    dr->dataCurr.humidity = humidity;
    deltaPressure = pressure-dr->dataCurr.pressure;
    dr->dataCurr.pressure = pressure;
    dr->dataCurr.batteryVoltage = batteryVoltage;
    dr->dataCurr.txpower = txpower;
    dr->dataCurr.movementCounter = movementCounter;
    deltaRssi = rssi-dr->dataCurr.rssi;
    dr->dataCurr.rssi = rssi;
    if (measurementSequence != dr->dataCurr.measurementSequence) {
        dr->updated++;
        // average temp for influx
        if (dr->dataInflux.temperature < -900) dr->dataInflux.temperature = temperature; else { dr->dataInflux.temperature += temperature; dr->dataInflux.temperature = dr->dataInflux.temperature / 2; }
        VPRINTFN(3," temperature: %5.3f dataInflux.temperature: %5.3f",temperature,dr->dataInflux.temperature);
        // max humidity for influx
        if (humidity > dr->dataInflux.humidity) dr->dataInflux.humidity = humidity;
        dr->dataInflux.rssi = rssi;
        dr->dataInflux.batteryVoltage = batteryVoltage;
        LOGN(1,"%012lx (%s): temp: %5.2f (%7.4f), humidity: %6.3f (%8.4f), pressure: %6d (%6d), batt: %5.2fV, txPower: %ddBm, rssi: %3d (%3d) mover: %d, seq: %d",macAddress,nm!=NULL?nm->name:NULL,temperature,deltaTemperature,humidity,deltaHumidity,pressure,deltaPressure,(double)batteryVoltage / 1000,txpower, rssi, deltaRssi, movementCounter, measurementSequence);
    } else {
    	VPRINTFN(3," received same sequence, temperature: %5.3f dataInflux.temperature: %5.3f",temperature,dr->dataInflux.temperature);
    }
    dr->dataCurr.measurementSequence = measurementSequence;
    return true;
}



int msgarrvd(void *context, char *topicName, int topicLen, MQTTClient_message *message)
{
	char *tokenID;
	cJSON *jmsg;
	cJSON *data = NULL;
	cJSON *rssi = NULL;
	int rssiValue = 0;

	VPRINTFN(3,"S: msgarrvd, topicLen: %d",topicLen);
	tokenID = strrchr(topicName,'/');
	if (tokenID) {
		tokenID++;
		if (strcmp(tokenID,"gw_status") != 0) {

			VPRINTF(2,"Message arrived\n");
			VPRINTF(2,"   topic: %s\n", topicName);
			VPRINTF(2," tokenID: %s\n", tokenID);
			VPRINTF(2," message: %.*s\n", message->payloadlen, (char*)message->payload);

			jmsg = cJSON_ParseWithLength((char*)message->payload, message->payloadlen);

			VPRINTF(3,"cJSON_Print:\n%s\n",cJSON_Print(jmsg));

			rssi = cJSON_GetObjectItemCaseSensitive(jmsg, "rssi");
			if (data == rssi) {
				EPRINTFN("%s: cJSON_GetObjectItemCaseSensitive (rssi) returned NULL, token id: \"%s\"",__PRETTY_FUNCTION__,tokenID);
			} else {
                if (cJSON_IsNumber(rssi)) {
                    rssiValue = rssi->valueint;
                } else
                    EPRINTFN("%s: number for rssi expected, token id: \"%s\"",__PRETTY_FUNCTION__,tokenID);
            }
			data = cJSON_GetObjectItemCaseSensitive(jmsg, "data");
			if (data == NULL) {
				EPRINTFN("%s: cJSON_GetObjectItemCaseSensitive (data) returned NULL, token id: \"%s\"",__PRETTY_FUNCTION__,tokenID);
			} else {

                //printf("data: \"%s\"\n",data->string);
                if (cJSON_IsString(data) && (data->valuestring != NULL)) {
					mqttDataLock();
                    processRuuviData(data->valuestring, rssiValue);
					mqttDataUnlock();
                } else {
					EPRINTFN("Error, data is NULL or not a string, data: '%s'",data->valuestring);
                }
			}

			cJSON_Delete(jmsg);
		} else {
			LOGN(3,"topic gw_status ignored");
		}
	} else {
		LOGN(3,"topicName without / (%s) ignored",topicName);
	}
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    VPRINTFN(3,"E: msgarrvd");
    return 1;
}

int mqttReceiverConnectionLost;

void connlost(void *context, char *cause) {
	EPRINTFN("Connection lost, will try to reconnect");
    //EPRINTFN("Connection lost (%s), will try to reconnect",cause);
    mqttReceiverConnectionLost++;
}

pthread_mutex_t mqttLock;
int mqttMutexCreated;

void mqttDataLock() {
	pthread_mutex_lock(&mqttLock);
}

void mqttDataUnlock() {
	pthread_mutex_unlock(&mqttLock);
}

#define QOS         1

int mqttReceiverInit (const char *hostname, int port, const char *topic, const char *clientID) {
time_t t = time(NULL);
struct tm tm = *localtime(&t);
char newClientID[512];

    sprintf(newClientID,"%s-%04d%02d%02d-%02d%02d%02d", clientID, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

    if (mqttMutexCreated == 0) {
		if (pthread_mutex_init(&mqttLock, NULL) != 0) {
			EPRINTFN("mqttReceiverInit: mutex init failed\n");
			exit (1);
		}
		mqttMutexCreated++;
    }

    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    int rc;
    char *address;

    address = (char *)calloc(1,strlen(hostname+20));
    sprintf(address,"%s:%d",hostname,port);
    EPRINTF("Connecting to MQTT server %s with client id '%s' and topic '%s'",address,newClientID,topic);
    if ((rc = MQTTClient_create(&client, address, newClientID, MQTTCLIENT_PERSISTENCE_NONE, NULL)) != MQTTCLIENT_SUCCESS) {
        EPRINTFN("Failed to create client for address %s, return code %d", address, rc);
        free(address);
        return 0;
    }

    if ((rc = MQTTClient_setCallbacks(client, NULL, connlost, msgarrvd, NULL)) != MQTTCLIENT_SUCCESS) {
        EPRINTFN("Failed to set callbacks, return code %d", rc);
        free(address);
        return 0;
    }

    opts.keepAliveInterval = 20;
    opts.cleansession = 1;
    if ((rc = MQTTClient_connect(client, &opts)) != MQTTCLIENT_SUCCESS)  {
        EPRINTFN("Failed to connect to %s, return code %d", address, rc);
        free(address);
        return 0;
    }
    LOGN(0,"connected to source MQTT server %s",address);
	free(address);
    if ((rc = MQTTClient_subscribe(client, topic, QOS)) != MQTTCLIENT_SUCCESS) {
        EPRINTFN("Failed to subscribe to topic \"%s\", return code %d\n", topic, rc);
        return 0;
    }
    LOGN(0,"subscribed to \"%s\", QOS: %d",topic,QOS);

	return true;
}


int mqttReceiverDone(const char *topic) {
	int rc;

	if ((rc = MQTTClient_unsubscribe(client, topic)) != MQTTCLIENT_SUCCESS) {
		EPRINTFN("Failed to unsubscribe, return code %d", rc);
	}


    if ((rc = MQTTClient_disconnect(client, 10000)) != MQTTCLIENT_SUCCESS) {
        EPRINTFN("Failed to disconnect, return code %d", rc);
    }

    MQTTClient_destroy(&client);
    return 1;
}
