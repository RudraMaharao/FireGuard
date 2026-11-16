#include <time.h>
#include "device_status.h"
#include "alerts.h"
#include "cloud_link.h"
#include "geolocate.h"
#include "sensors.h"
#include "vision_service.h"
#include "sms.h"
#include "storage.h"
#include "wifi_sync.h"
cJSON *device_status_json() {
    auto s = sensors::snapshot();
    auto h = alerts::health();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "state", STATE_NAME[s.state]);
    cJSON_AddStringToObject(j,"hardwareRevision","two-mq-ads1115-v2");
    cJSON_AddStringToObject(j,"softwareRevision","hybrid-location-v3.7-snapshot");
    cJSON_AddStringToObject(j,"sensorModel","two-sensor-relative-prototype");
    auto cameraHealth=vision_service::health();
    cJSON_AddBoolToObject(j,"cameraBusy",cameraHealth.busy);
    cJSON_AddBoolToObject(j,"cameraReady",cameraHealth.ready);
    cJSON_AddNumberToObject(j,"cameraImageAt",cameraHealth.imageAt);
    cJSON_AddNumberToObject(j,"cameraAttemptAt",cameraHealth.attemptedAt);
    cJSON_AddStringToObject(j,"cameraError",cameraHealth.reason);
    auto cloudHealth=cloud_link::health();
    cJSON_AddNumberToObject(j,"cloudLastStatus",cloudHealth.status);
    cJSON_AddNumberToObject(j,"cloudLastError",cloudHealth.error);
    cJSON_AddNumberToObject(j,"cloudLastSuccessMs",cloudHealth.lastSuccessMs);
    cJSON_AddNumberToObject(j,"telegramLastStatus",h.httpStatus);


    cJSON_AddBoolToObject(j,"coAvailable",false);
    cJSON_AddBoolToObject(j,"ads1115",s.adcOK);
    cJSON_AddBoolToObject(j,"mq2Valid",s.mq2OK);
    cJSON_AddBoolToObject(j,"mq135Valid",s.mq135OK);
    cJSON_AddNumberToObject(j,"mq2AdcMv",s.adcMv2);
    cJSON_AddNumberToObject(j,"mq135AdcMv",s.adcMv135);
    cJSON_AddNumberToObject(j,"mq2AoMv",s.adcMv2*2);
    cJSON_AddNumberToObject(j,"mq135AoMv",s.adcMv135*2);
    cJSON_AddNumberToObject(j,"mq2Deviation",s.dev[0]);
    cJSON_AddNumberToObject(j,"mq135Deviation",s.dev[2]);
#define NUMBER(name, value) cJSON_AddNumberToObject(j, name, value)
#define BOOL(name, value) cJSON_AddBoolToObject(j, name, value)
    NUMBER("frs", s.frs);
    NUMBER("grs", s.grs);
    NUMBER("crs", s.crs);
    NUMBER("temperature", s.tC);
    NUMBER("humidity", s.rh);
    NUMBER("pressure", s.hPa);
    BOOL("cameraRequested", s.cameraRequested);
    BOOL("cameraValid", s.cameraValid);
    NUMBER("visionAgeMs", s.visionAgeMs);
    BOOL("muted", s.muted);
    BOOL("sensorError", s.flags & 1);
    BOOL("warming", s.flags & 2);
    BOOL("calibrated", s.calibrated);
    NUMBER("burninHours", s.burninSec / 3600.0);
    NUMBER("label", s.label);
    BOOL("wifi", wifi_sync::connected());
    BOOL("telegram", h.internet);
    NUMBER("registered", h.registered);
    NUMBER("sent", h.sent);
    NUMBER("failed", h.failed);
    NUMBER("dropped", h.dropped);
    NUMBER("samples", storage::count());
    NUMBER("event", h.event);
    BOOL("acknowledged", h.acknowledged);
    BOOL("forwarded", h.forwarded);
    auto m = sms::health();
    BOOL("smsEnabled", m.enabled && m.configured);
    NUMBER("smsSent", m.sent);
    NUMBER("smsFailed", m.failed);
    NUMBER("smsSkipped", m.skipped);
    NUMBER("smsLastStatus", m.lastStatus);
    BOOL("stationSms",m.station); NUMBER("stationSmsSent",m.stationSent);
    auto where=geolocate::current(); auto g=geolocate::health();
    cJSON *loc=cJSON_AddObjectToObject(j,"location");
    cJSON_AddStringToObject(loc,"source",location_policy::source_name(where.source));
    cJSON_AddBoolToObject(loc,"valid",where.valid);
    cJSON_AddNumberToObject(loc,"accuracy",where.accuracyM);
    cJSON_AddBoolToObject(loc,"mismatch",where.mismatch);
    cJSON_AddBoolToObject(loc,"wifiEnabled",g.enabled&&g.configured);
    cJSON_AddBoolToObject(loc,"wifiFix",g.fix.valid&&location_policy::fresh(g.fix,time(nullptr)));
    if(where.valid){
        cJSON_AddNumberToObject(loc,"latitude",where.lat);
        cJSON_AddNumberToObject(loc,"longitude",where.lng);
        cJSON_AddStringToObject(loc,"map",location_policy::maps_link(where.lat,where.lng).c_str());
    }
#undef NUMBER
#undef BOOL
    return j;
}
