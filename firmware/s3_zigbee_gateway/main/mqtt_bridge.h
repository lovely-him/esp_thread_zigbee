#pragma once

#include "zb_dev.h"

void mqtt_bridge_start(void);
void mqtt_bridge_on_state(const zb_dev_t *dev);
