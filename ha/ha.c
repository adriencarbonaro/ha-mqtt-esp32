#include "ha.h"
#include "esp_log.h"
#include "mqtt.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISCOVERY_PREFIX "homeassistant"

typedef struct
{
    char* buf;
    size_t len;
    size_t cap;
    bool ok;
} sb_t;

/* Static objects *************************************************************/

static const char* TAG = "ha";

static const ha_identity_t* s_identity = NULL;

static char s_avail_topic[64];

static void ha_register_identity(const ha_identity_t* identity)
{
    if (identity == NULL) ESP_LOGW(TAG, "identity registered is NULL");
    s_identity = identity;
    snprintf(s_avail_topic,
             sizeof(s_avail_topic),
             "%s/availability",
             s_identity->device_id);
    mqtt_set_lwt(s_avail_topic);
}

static const ha_entity_t* s_entities[CONFIG_HA_MAX_ENTITIES];
static size_t s_entity_count = 0;

static const ha_entity_t s_version_entity = {
    .id = "version",
    .name = "Version",
    .platform = HA_SENSOR,
    .state_topic = "version",
    .entity_category = "diagnostic",
    .icon = "mdi:git",
};

static int ha_register_entity(const ha_entity_t* entity)
{
    if (entity == NULL) return -1;

    if (s_entity_count >= CONFIG_HA_MAX_ENTITIES)
    {
        ESP_LOGE(TAG,
                 "entity table full (%d), dropping '%s'",
                 CONFIG_HA_MAX_ENTITIES,
                 entity->id);
        return -1;
    }

    s_entities[s_entity_count++] = entity;
    return 0;
}

static int ha_register_entities(const ha_entity_t* entities, size_t count)
{
    if (entities == NULL) return -1;

    for (size_t i = 0; i < count; i++)
    {
        if (ha_register_entity(&entities[i]) != 0) return -1;
    }
    return 0;
}

/* The topic an entity's state is published to is relative to the device id;
 * a NULL state_topic defaults to the entity id. */
static const char* entity_state_topic(const ha_entity_t* e)
{
    return e->state_topic ? e->state_topic : e->id;
}

static void sb_reserve(sb_t* s, size_t extra)
{
    if (!s->ok || s->len + extra + 1 <= s->cap) return;

    size_t cap = s->cap ? s->cap : 512;
    while (cap < s->len + extra + 1) cap *= 2;

    char* buf = realloc(s->buf, cap);
    if (buf == NULL)
    {
        s->ok = false;
        return;
    }
    s->buf = buf;
    s->cap = cap;
}

static void sb_put(sb_t* s, const char* str)
{
    if (str == NULL) return;

    size_t n = strlen(str);
    sb_reserve(s, n);
    if (!s->ok) return;

    memcpy(s->buf + s->len, str, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

/* Append a "key":"value", member (trailing comma), skipped when value is
 * NULL. Values come from static project config and are not JSON-escaped. */
static void sb_field(sb_t* s, const char* key, const char* value)
{
    if (value == NULL) return;

    sb_put(s, "\"");
    sb_put(s, key);
    sb_put(s, "\":\"");
    sb_put(s, value);
    sb_put(s, "\",");
}

/* Drop a single trailing ',' if present. */
static void sb_trim_comma(sb_t* s)
{
    if (s->ok && s->len > 0 && s->buf[s->len - 1] == ',')
        s->buf[--s->len] = '\0';
}

static const char* platform_str(ha_platform_t platform)
{
    switch (platform)
    {
        case HA_BINARY_SENSOR:
            return "binary_sensor";
        case HA_SWITCH:
            return "switch";
        case HA_BUTTON:
            return "button";
        case HA_NUMBER:
            return "number";
        case HA_SELECT:
            return "select";
        case HA_SENSOR:
        default:
            return "sensor";
    }
}

/* Append one entity as a member of the discovery "components" object. */
static void append_entity(sb_t* s, const char* device_id, const ha_entity_t* e)
{
    /* component key + unique_id + object_id: "<device id>_<entity id>" */
    char uid[96];
    snprintf(uid, sizeof(uid), "%s_%s", device_id, e->id);

    sb_put(s, "\"");
    sb_put(s, uid);
    sb_put(s, "\":{");

    sb_field(s, "platform", platform_str(e->platform));
    sb_field(s, "name", e->name);
    sb_field(s, "unique_id", uid);
    sb_field(s, "object_id", uid);

    char topic[160];

    /* A button is stateless: HA's button schema has no state_topic and
     * rejects the whole discovery config if one is present. */
    if (e->platform != HA_BUTTON)
    {
        snprintf(topic,
                 sizeof(topic),
                 "%s/%s",
                 device_id,
                 entity_state_topic(e));
        sb_field(s, "state_topic", topic);
    }

    if (e->command_topic)
    {
        snprintf(topic, sizeof(topic), "%s/%s", device_id, e->command_topic);
        sb_field(s, "command_topic", topic);
    }

    sb_field(s, "device_class", e->device_class);
    sb_field(s, "unit_of_measurement", e->unit);
    sb_field(s, "state_class", e->state_class);
    sb_field(s, "entity_category", e->entity_category);
    sb_field(s, "icon", e->icon);
    sb_field(s, "value_template", e->value_template);

    /* sb_field always leaves a trailing comma (platform is never NULL):
     * extra slots straight in after it, otherwise the comma is dropped. */
    if (e->extra)
        sb_put(s, e->extra);
    else
        sb_trim_comma(s);

    sb_put(s, "}");
}

static char s_cmd_topics[CONFIG_HA_MAX_COMMANDS][160];
static const ha_entity_t* s_cmd_entities[CONFIG_HA_MAX_COMMANDS];
static mqtt_subscription_t s_cmd_subs[CONFIG_HA_MAX_COMMANDS];
static uint16_t s_cmd_count = 0;

/* The single mqtt handler shared by every command topic: resolve the topic
 * back to its entity and hand on_command a NUL-terminated payload copy (the
 * mqtt payload is a length-bounded slice, not a C string). */
static void command_dispatch(const char* topic,
                             uint16_t topic_len,
                             const char* msg,
                             uint16_t msg_len)
{
    for (uint16_t i = 0; i < s_cmd_count; i++)
    {
        if (strlen(s_cmd_topics[i]) != topic_len ||
            memcmp(s_cmd_topics[i], topic, topic_len) != 0)
            continue;

        char payload[64];
        size_t n =
            msg_len < sizeof(payload) - 1 ? msg_len : sizeof(payload) - 1;
        memcpy(payload, msg, n);
        payload[n] = '\0';

        const ha_entity_t* e = s_cmd_entities[i];
        ESP_LOGI(TAG, "command for '%s': %s", e->id, payload);
        e->on_command(e->id, payload);
        return;
    }
}

static int ha_subscribe_commands(void)
{
    s_cmd_count = 0;

    for (size_t i = 0; i < s_entity_count; i++)
    {
        const ha_entity_t* e = s_entities[i];
        if (e->command_topic == NULL || e->on_command == NULL) continue;

        if (s_cmd_count >= CONFIG_HA_MAX_COMMANDS)
        {
            ESP_LOGE(TAG,
                     "command table full (%d), '%s' stays read-only",
                     CONFIG_HA_MAX_COMMANDS,
                     e->id);
            return -1;
        }

        snprintf(s_cmd_topics[s_cmd_count],
                 sizeof(s_cmd_topics[0]),
                 "%s/%s",
                 s_identity->device_id,
                 e->command_topic);
        s_cmd_subs[s_cmd_count].topic = s_cmd_topics[s_cmd_count];
        s_cmd_subs[s_cmd_count].handler = command_dispatch;
        s_cmd_entities[s_cmd_count] = e;
        s_cmd_count++;
    }

    mqtt_set_subscriptions(s_cmd_subs, s_cmd_count);
    ESP_LOGI(TAG, "%u command topic(s) subscribed", (unsigned)s_cmd_count);
    return 0;
}

/* Public functions ***********************************************************/

void ha_publish_discovery(void)
{
    if (s_identity == NULL)
    {
        ESP_LOGE(TAG, "Identity is NULL, cannot publish discovery");
        return;
    }

    const char* device_id = s_identity->device_id;

    sb_t s = {.ok = true};
    sb_reserve(&s, 1024);

    /* device block */
    sb_put(&s, "{\"device\":{\"identifiers\":[\"");
    sb_put(&s, device_id);
    sb_put(&s, "\"],");
    sb_field(&s, "name", s_identity->device_name);
    sb_field(&s, "manufacturer", s_identity->manufacturer);
    sb_field(&s, "model", s_identity->model);
    sb_field(&s, "sw_version", s_identity->version_str);
    sb_trim_comma(&s);
    sb_put(&s, "},");

    /* origin block */
    sb_put(&s, "\"origin\":{");
    sb_field(&s, "name", s_identity->device_name);
    sb_field(&s, "sw_version", s_identity->version_str);
    sb_trim_comma(&s);
    sb_put(&s, "},");

    /* availability block */
    sb_put(&s, "\"availability\":[{\"topic\":\"");
    sb_put(&s, s_avail_topic);
    sb_put(&s,
           "\",\"payload_available\":\"online\","
           "\"payload_not_available\":\"offline\"}],");

    /* components: project entities, then the free version sensor */
    sb_put(&s, "\"components\":{");
    for (size_t i = 0; i < s_entity_count; i++)
    {
        append_entity(&s, device_id, s_entities[i]);
        sb_put(&s, ",");
    }
    append_entity(&s, device_id, &s_version_entity);
    sb_put(&s, "}}");

    if (!s.ok)
    {
        ESP_LOGE(TAG, "discovery payload allocation failed");
        free(s.buf);
        return;
    }

    char topic[160];
    snprintf(topic,
             sizeof(topic),
             "%s/device/%s/config",
             DISCOVERY_PREFIX,
             device_id);

    ESP_LOGI(TAG,
             "publishing HA discovery (%u entities, %u bytes)",
             (unsigned)(s_entity_count + 1),
             (unsigned)s.len);
    mqtt_publish_retained(topic, s.buf);
    free(s.buf);

    /* Firmware version value for the free version sensor. */
    char version_topic[96];
    snprintf(version_topic, sizeof(version_topic), "%s/version", device_id);
    mqtt_publish_retained(version_topic, s_identity->version_str);
}

void ha_publish(const char* entity_id, const char* value)
{
    if (entity_id == NULL || value == NULL) return;

    for (size_t i = 0; i < s_entity_count; i++)
    {
        const ha_entity_t* e = s_entities[i];
        if (strcmp(e->id, entity_id) != 0) continue;

        char topic[160];
        snprintf(topic,
                 sizeof(topic),
                 "%s/%s",
                 s_identity->device_id,
                 entity_state_topic(e));
        mqtt_publish(topic, value);
        return;
    }

    ESP_LOGD(TAG, "ha_publish: no entity '%s'", entity_id);
}

void ha_init(const ha_identity_t* identity,
             const ha_entity_t* entities,
             uint16_t nb_entities)
{
    ha_register_identity(identity);
    ha_register_entities(entities, nb_entities);
    ha_subscribe_commands();
}
