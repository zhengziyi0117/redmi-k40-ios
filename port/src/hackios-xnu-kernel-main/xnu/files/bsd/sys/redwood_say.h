/* The Wi-Fi log's two kinds of line (sweet_wifi.c): both go to the kernel log here. */
#ifndef _REDWOOD_SAY_H_
#define _REDWOOD_SAY_H_
int alioth_wlan_wifi_say(const char *format, ...) __printflike(1, 2);      /* a line of the Wi-Fi log */
int alioth_wlan_wifi_tell(const char *format, ...) __printflike(1, 2);     /* ... one always said */
#endif
