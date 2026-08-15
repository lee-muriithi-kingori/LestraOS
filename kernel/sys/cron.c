/*
 * Lestra OS - Cron Daemon
 * Copyright (c) 2026 lestramk.org
 *
 * A simple scheduled-task runner. Tasks fire on minute boundaries
 * when the current time matches all five schedule fields. Each task
 * runs a shell command line via shell_execute_line(). Max 16 tasks.
 *
 * Schedule format (5 whitespace-separated fields):
 *   minute  hour  day-of-month  month  day-of-week
 *   0-59    0-23  1-31           1-12   0-6 (0 = Sunday)
 * Each field is either "*" (wildcard) or a non-negative integer in
 * the field's valid range.
 *
 * cron_tick() is invoked from the timer IRQ every second; we record
 * the last minute a task fired in so we don't re-fire it multiple
 * times within the same wall-clock minute.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/cron.h>
#include <lestra/timer.h>
#include <string.h>

#define CRON_MAX_TASKS  16
#define CRON_STR_LEN    128

/* Wildcard sentinel for schedule fields. */
#define CRON_WILDCARD  (-1)

typedef struct {
    int     active;          /* 1 = slot in use                      */
    int     id;              /* User-visible task ID                 */
    int     minute;          /* 0-59 or CRON_WILDCARD                */
    int     hour;            /* 0-23 or CRON_WILDCARD                */
    int     day;             /* 1-31 or CRON_WILDCARD                */
    int     month;           /* 1-12 or CRON_WILDCARD                */
    int     weekday;         /* 0-6 (0=Sunday) or CRON_WILDCARD      */
    char    schedule[CRON_STR_LEN];
    char    command[CRON_STR_LEN];
    /* Last-fire stamp to suppress double-firing in the same minute. */
    uint16_t last_year;
    uint8_t  last_month;
    uint8_t  last_day;
    uint8_t  last_hour;
    uint8_t  last_min;
} cron_task_t;

static cron_task_t tasks[CRON_MAX_TASKS];
static int next_id    = 1;
static int initialized = 0;

/* Externs we rely on. */
extern void shell_execute_line(const char* line, void (*out_cb)(char));
extern void rtc_get_time(uint8_t* hour, uint8_t* min, uint8_t* sec);
extern void rtc_get_date(uint16_t* year, uint8_t* month, uint8_t* day);

/* Parse a single schedule field. Returns the integer value, CRON_WILDCARD
 * for "*", or -2 on parse error. */
static int parse_field(const char* tok, int min_val, int max_val) {
    if (tok[0] == '*' && tok[1] == '\0') {
        return CRON_WILDCARD;
    }
    int v = 0;
    int any = 0;
    for (const char* p = tok; *p; p++) {
        if (*p < '0' || *p > '9') {
            return -2;   /* Invalid character */
        }
        v = v * 10 + (*p - '0');
        any = 1;
    }
    if (!any) return -2;
    if (v < min_val || v > max_val) {
        return -2;
    }
    return v;
}

/* Parse "M H D MON WDW" into a cron_task_t's schedule fields.
 * Returns 0 on success, -1 on parse error. */
static int parse_schedule(cron_task_t* t, const char* sched) {
    char buf[CRON_STR_LEN];
    size_t n = strnlen(sched, CRON_STR_LEN);
    if (n >= CRON_STR_LEN) return -1;
    memcpy(buf, sched, n);
    buf[n] = '\0';

    /* Split on whitespace into up to 5 tokens. */
    char* fields[5];
    int nf = 0;
    char* p = buf;
    while (*p && nf < 5) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        fields[nf++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
    }
    if (nf != 5) return -1;

    int v;
    v = parse_field(fields[0], 0, 59); if (v < -1) return -1; t->minute  = v;
    v = parse_field(fields[1], 0, 23); if (v < -1) return -1; t->hour    = v;
    v = parse_field(fields[2], 1, 31); if (v < -1) return -1; t->day     = v;
    v = parse_field(fields[3], 1, 12); if (v < -1) return -1; t->month   = v;
    v = parse_field(fields[4], 0, 6);  if (v < -1) return -1; t->weekday = v;

    return 0;
}

/* Day-of-week for a Gregorian date: 0=Sunday ... 6=Saturday.
 * Uses Zeller's congruence. */
static int compute_weekday(int year, int month, int day) {
    /* Zeller expects March=3 .. February=14; adjust Jan/Feb. */
    int m = month;
    int y = year;
    if (m < 3) { m += 12; y -= 1; }
    int k = y % 100;
    int j = y / 100;
    /* h = (q + floor(13(m+1)/5) + K + floor(K/4) + floor(J/4) + 5J) mod 7
     * (5J is equivalent to -2J mod 7, avoids negative operands.) */
    int h = (day + (13 * (m + 1)) / 5 + k + (k / 4) + (j / 4) + 5 * j) % 7;
    /* Zeller: h=0 -> Saturday, h=1 -> Sunday, ... h=6 -> Friday.
     * Convert to Sunday=0 .. Saturday=6. */
    return ((h + 6) % 7);
}

void cron_init(void) {
    pr_info("cron: initialising (max %d tasks)\n", CRON_MAX_TASKS);
    memset(tasks, 0, sizeof(tasks));
    next_id = 1;
    initialized = 1;
    pr_info("cron: ready\n");
}

int cron_add(const char* schedule, const char* command) {
    if (!initialized || !schedule || !command) return -1;

    /* Find a free slot. */
    int slot = -1;
    for (int i = 0; i < CRON_MAX_TASKS; i++) {
        if (!tasks[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        pr_warn("cron: table full, cannot add task\n");
        return -1;
    }

    /* Validate lengths. */
    if (strnlen(schedule, CRON_STR_LEN) >= CRON_STR_LEN ||
        strnlen(command, CRON_STR_LEN) >= CRON_STR_LEN) {
        pr_warn("cron: schedule or command too long (max %d)\n",
                CRON_STR_LEN - 1);
        return -1;
    }

    cron_task_t* t = &tasks[slot];
    memset(t, 0, sizeof(*t));
    if (parse_schedule(t, schedule) != 0) {
        pr_warn("cron: invalid schedule \"%s\"\n", schedule);
        return -1;
    }
    strcpy(t->schedule, schedule);
    strcpy(t->command, command);
    t->id     = next_id++;
    t->active = 1;
    /* last_fire_* left at 0 — first match in a real minute fires. */

    pr_info("cron: added task #%d \"%s\" -> \"%s\"\n",
            t->id, t->schedule, t->command);
    return t->id;
}

int cron_remove(int id) {
    if (!initialized) return -1;
    for (int i = 0; i < CRON_MAX_TASKS; i++) {
        if (tasks[i].active && tasks[i].id == id) {
            pr_info("cron: removed task #%d\n", id);
            memset(&tasks[i], 0, sizeof(tasks[i]));
            return 0;
        }
    }
    pr_warn("cron: task #%d not found\n", id);
    return -1;
}

void cron_list(void) {
    if (!initialized) {
        pr_info("cron: not initialised\n");
        return;
    }
    pr_info("cron: %d task(s) scheduled\n", cron_count());
    int count = 0;
    for (int i = 0; i < CRON_MAX_TASKS; i++) {
        if (!tasks[i].active) continue;
        pr_info("  #%d  %s  %s\n",
                tasks[i].id, tasks[i].schedule, tasks[i].command);
        count++;
    }
    if (count == 0) {
        pr_info("  (none)\n");
    }
}

int cron_count(void) {
    int n = 0;
    for (int i = 0; i < CRON_MAX_TASKS; i++) {
        if (tasks[i].active) n++;
    }
    return n;
}

/* Check whether a task's schedule matches the current time, and if so
 * (and we haven't already fired this minute) execute it. */
static void cron_check_task(cron_task_t* t,
                            uint16_t year, uint8_t month, uint8_t day,
                            uint8_t hour, uint8_t min) {
    if (t->minute  != CRON_WILDCARD && t->minute  != (int)min)   return;
    if (t->hour    != CRON_WILDCARD && t->hour    != (int)hour)  return;
    if (t->day     != CRON_WILDCARD && t->day     != (int)day)   return;
    if (t->month   != CRON_WILDCARD && t->month   != (int)month) return;

    int wd = compute_weekday((int)year, (int)month, (int)day);
    if (t->weekday != CRON_WILDCARD && t->weekday != wd)         return;

    /* Suppress double-firing within the same wall-clock minute. */
    if (t->last_year  == year  &&
        t->last_month == month &&
        t->last_day   == day   &&
        t->last_hour  == hour  &&
        t->last_min   == min) {
        return;
    }

    pr_info("cron: firing #%d \"%s\" -> \"%s\"\n",
            t->id, t->schedule, t->command);

    t->last_year  = year;
    t->last_month = month;
    t->last_day   = day;
    t->last_hour  = hour;
    t->last_min   = min;

    /* Execute the command. NULL output callback means the shell uses
     * its default sink (terminal / printk). */
    shell_execute_line(t->command, NULL);
}

void cron_tick(void) {
    if (!initialized) return;

    uint8_t  hour, min, sec;
    uint16_t year;
    uint8_t  month, day;
    rtc_get_time(&hour, &min, &sec);
    rtc_get_date(&year, &month, &day);

    for (int i = 0; i < CRON_MAX_TASKS; i++) {
        if (!tasks[i].active) continue;
        cron_check_task(&tasks[i], year, month, day, hour, min);
    }
}
