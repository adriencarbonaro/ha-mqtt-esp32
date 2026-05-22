cmake_minimum_required(VERSION 3.25)

idf_component_register(
    SRCS
        ha/ha.c
        mqtt/mqtt.c

    INCLUDE_DIRS
        "ha"
        "mqtt"

    REQUIRES
        mqtt
)
