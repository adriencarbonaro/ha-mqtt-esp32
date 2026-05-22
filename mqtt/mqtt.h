#ifndef MQTT_H_
#define MQTT_H_

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"

/* A topic subscription and the handler invoked when a message arrives. */
typedef struct
{
    const char* topic;
    void (*handler)(const char* topic,
                    uint16_t topic_len,
                    const char* msg,
                    uint16_t msg_len);
} mqtt_subscription_t;

/* Called once every time the MQTT client (re)connects. */
typedef void (*mqtt_on_connect_cb_t)(void);

/* Optional subscription table, (re)applied on every connect. The table is
 * referenced, not copied. Set before mqtt_start(). */
void mqtt_set_subscriptions(const mqtt_subscription_t* table, uint16_t len);

/* Callback fired on every (re)connect, after subscriptions and the
 * availability "online" publish. Set before mqtt_start(). */
void mqtt_set_on_connect(mqtt_on_connect_cb_t cb);

/* Set Last Will Topic (LWT). */
void mqtt_set_lwt(const char* lwt_topic);

/* Connect to the broker (CONFIG_MQTT_BROKER_URI) once wifi is up.
 * If lwt_topic is non-NULL it is used as the Last-Will / availability topic:
 * "offline" is the retained will, and "online" is published retained on
 * every connect. The string must stay valid for the program lifetime. */
void mqtt_start(void);

void mqtt_publish(const char* topic, const char* msg);

/* Like mqtt_publish() but QoS 1 + retained (state / discovery topics). */
int mqtt_publish_retained(const char* topic, const char* msg);

#endif /* MQTT_H_ */
