#ifndef LESTRA_POWER_H
#define LESTRA_POWER_H
int battery_init(void);
int battery_get_percent(void);
int battery_is_charging(void);
const char* battery_get_status_str(void);
int temp_init(void);
int temp_get_cpu(void);
int temp_get_gpu(void);
#endif
