#ifndef RUUVIMQTT_H_INCLUDED
#define RUUVIMQTT_H_INCLUDED

#include <stdint.h>

typedef struct nameMappings_t nameMappings_t;
struct nameMappings_t {
	int64_t mac;
	char *name;
	nameMappings_t *next;
};


typedef struct unmappedDevices_t unmappedDevices_t;
struct unmappedDevices_t {
	int64_t mac;
	unmappedDevices_t *next;
};


typedef struct sensorData_t sensorData_t;
struct sensorData_t {
	double temperature,humidity;
	int pressure,batteryVoltage,txpower,movementCounter,measurementSequence,rssi;
};

typedef struct dataRead_t dataRead_t;
struct dataRead_t {
        int64_t mac;
		char *tokenID;
		char *name;
		char *rawData;

		sensorData_t dataCurr;
		sensorData_t dataLastSent;
		sensorData_t dataInflux;
        int updated;

        dataRead_t *next;
};

extern dataRead_t *mqttDataRead;

int64_t hex2int (const char *src, int nibbles, int isSigned);

// 1=success
int addMapping (const char *tokenMac, const char *name);

extern nameMappings_t *nameMappings;
extern dataRead_t *mqttDataRead;

int mqttReceiverInit (const char *hostname, int port, const char *topic, const char *clientID);
int mqttReceiverDone (const char *topic);
int mqttReceiver_isConnected();

#endif // RUUVIMQTT_H_INCLUDED
