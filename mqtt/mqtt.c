#include "mqtt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "ha.h"
#include "sdkconfig.h"
#include <string.h>

extern void ha_publish_discovery(void);

/* Static objects ----------------------------------------------------------- */
static const char* TAG = "mqtt";
static esp_mqtt_client_handle_t mqtt_client = NULL;
static const mqtt_subscription_t* s_subs = NULL;
static uint16_t s_subs_len = 0;
static mqtt_on_connect_cb_t s_on_connect = NULL;
static const char* s_lwt_topic = NULL;

/* Static functions --------------------------------------------------------- */
static void mqtt_event_handler(void* event_handler_arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void* event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to server");

            for (uint16_t i = 0; i < s_subs_len; i++)
            {
                const mqtt_subscription_t* sub = &s_subs[i];
                if (sub == NULL) continue;

                ESP_LOGI(TAG, "Subscribing to topic %s", sub->topic);
                esp_mqtt_client_subscribe(event->client, sub->topic, 0);
            }

            if (s_lwt_topic) mqtt_publish_retained(s_lwt_topic, "online");

            ha_publish_discovery();

            if (s_on_connect) s_on_connect();

            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Disconnected from server");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "Subscription success (msg_id=%d)", event->msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "Unsubscription success (msg_id=%d)", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "Published, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG,
                     "Received message on topic %.*s: %.*s",
                     event->topic_len,
                     event->topic,
                     event->data_len,
                     event->data);

            for (uint16_t i = 0; i < s_subs_len; i++)
            {
                const mqtt_subscription_t* sub = &s_subs[i];
                if (sub != NULL)
                {
                    if (strlen(sub->topic) == event->topic_len &&
                        !memcmp(sub->topic, event->topic, event->topic_len))
                    {
                        sub->handler(event->topic,
                                     event->topic_len,
                                     event->data,
                                     event->data_len);
                    }
                }
            }
            break;

        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGI(TAG, "Before connect...");
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "Error");
            break;

        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
}

/* Public functions --------------------------------------------------------- */
void mqtt_publish(const char* topic, const char* msg)
{
    if (mqtt_client)
    {
        ESP_LOGI(TAG, "Publishing (topic=%s): %s", topic, msg);
        esp_mqtt_client_publish(mqtt_client, topic, msg, 0, 0, 0);
    }
}

int mqtt_publish_retained(const char* topic, const char* msg)
{
    if (!mqtt_client) return -1;

    ESP_LOGI(TAG, "Publishing retained (topic=%s): %s", topic, msg);
    return esp_mqtt_client_publish(mqtt_client, topic, msg, 0, 1, 1);
}

void mqtt_set_subscriptions(const mqtt_subscription_t* table, uint16_t len)
{
    s_subs = table;
    s_subs_len = len;
}

void mqtt_set_on_connect(mqtt_on_connect_cb_t cb) { s_on_connect = cb; }

void mqtt_set_lwt(const char* lwt_topic) { s_lwt_topic = lwt_topic; }

void mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
    };

    if (s_lwt_topic)
    {
        cfg.session.last_will.topic = s_lwt_topic;
        cfg.session.last_will.msg = "offline";
        cfg.session.last_will.qos = 1;
        cfg.session.last_will.retain = 1;
    }

    mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt_client,
                                   ESP_EVENT_ANY_ID,
                                   mqtt_event_handler,
                                   mqtt_client);

    esp_mqtt_client_start(mqtt_client);
}
