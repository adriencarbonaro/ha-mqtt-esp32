# ha-mqtt

ESP-IDF component that exposes the device to Home Assistant over MQTT.

Two layers:

- **`mqtt/`** — thin wrapper around `esp-mqtt`. Connects to
  `CONFIG_MQTT_BROKER_URI`, manages a subscription table that is re-applied on
  every (re)connect, supports an `online`/`offline` Last-Will availability
  topic, and exposes `mqtt_publish()` / `mqtt_publish_retained()`.
- **`ha/`** — Home Assistant integration on top of `mqtt/`. Caller registers an
  `ha_identity_t` (device id, name, manufacturer, model, version) and a table
  of `ha_entity_t` (sensors, binary sensors, switches, buttons, numbers,
  selects). `ha_init()`
  publishes the MQTT discovery configs, subscribes to command topics for
  entities with an `on_command` handler, and `ha_publish()` pushes state
  updates by entity id.

Bounds `CONFIG_HA_MAX_ENTITIES` and `CONFIG_HA_MAX_COMMANDS` are sized at
compile time via Kconfig.

## Usage

```c
#include "ha/ha.h"

static const ha_identity_t id = { .device_id = "dev1", .device_name = "...",
                                  .manufacturer = "...", .model = "...",
                                  .version_str = "1.0.0" };
static const ha_entity_t entities[] = { /* ... */ };

ha_init(&id, entities, sizeof(entities) / sizeof(entities[0]));
ha_publish("temp", "21.4");
```
