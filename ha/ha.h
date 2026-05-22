#ifndef HA_H_
#define HA_H_

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    const char* device_id;
    const char* device_name;
    const char* manufacturer;
    const char* model;
    const char* version_str;
} ha_identity_t;

typedef enum
{
    HA_SENSOR,
    HA_BINARY_SENSOR,
    HA_SWITCH,
    HA_BUTTON,
} ha_platform_t;

typedef void (*command_callback_t)(const char* id, const char* payload);

typedef struct
{
    const char* id;
    const char* name;
    ha_platform_t platform;
    const char* state_topic;
    const char* command_topic;
    command_callback_t on_command;
    const char* device_class;
    const char* unit;
    const char* state_class;
    const char* entity_category;
    const char* icon;
    const char* value_template;
    const char* extra;
} ha_entity_t;

/* Public functions ***********************************************************/

void ha_init(const ha_identity_t* identity,
             const ha_entity_t* entities,
             uint16_t nb_entities);

void ha_publish(const char* entity_id, const char* value);

#endif /* HA_H_ */
