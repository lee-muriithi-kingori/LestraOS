#ifndef LESTRA_WIFI_H
#define LESTRA_WIFI_H
void wifi_init(void);
int wifi_scan(void);
const char* wifi_get_ssid(int idx);
int wifi_get_signal(int idx);
int wifi_connect(const char* ssid, const char* password);
int wifi_disconnect(void);
int wifi_is_connected(void);
const char* wifi_get_connected_ssid(void);
#endif
