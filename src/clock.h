#ifndef CLOCK_H
#define CLOCK_H

#include "esp_err.h"

esp_err_t clock_init(void);
void clock_render(void);

#endif // CLOCK_H