#ifndef LESTRA_WIFI_H
#define LESTRA_WIFI_H

#include <lestra/types.h>

void wifi_init(void);
int wifi_scan(void);
const char* wifi_get_ssid(int idx);
int wifi_get_signal(int idx);
int wifi_connect(const char* ssid, const char* password);
int wifi_disconnect(void);
int wifi_is_connected(void);
const char* wifi_get_connected_ssid(void);

/* Called by net.c when a WLAN management frame (ethertype 0x88B4)
 * arrives. The payload is the raw802.11 management frame (after
 * stripping the Ethernet header). */
void wifi_handle_frame(const uint8_t* data, uint16_t len);

/* Called by net.c when an EAPOL frame (ethertype 0x888E) arrives.
 * Used for the WPA/WPA2 4-way handshake. */
void wifi_handle_eapol_frame(const uint8_t* data, uint16_t len);

#endif
