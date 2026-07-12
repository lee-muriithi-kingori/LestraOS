#ifndef LESTRA_CRON_H
#define LESTRA_CRON_H
void cron_init(void);
void cron_tick(void);
int cron_add(const char* schedule, const char* command);
int cron_remove(int id);
void cron_list(void);
int cron_count(void);
#endif
