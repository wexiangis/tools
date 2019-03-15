#ifndef __WLAN_H_
#define __WLAN_H_

#include <stdlib.h>
#include <stdbool.h>

//----- 全双工管道封装 -----

typedef struct{
	int fr, fw;
	pid_t pid;
	bool run;
}DuplexPipe;

bool screen_duplex_popen(DuplexPipe *dp, const char *cmd);
void screen_duplex_pclose(DuplexPipe *dp);

//----- wlan -----

//wifi 扫描每新增一条网络就回调该函数,由用户自行处理新增的网络
typedef void (*ScanCallback)(void *object, char *name, int keyType, int power);

void screen_wifi_scan(void *object, ScanCallback callback, int timeout);
void screen_wifi_connect(char *ssid, char *key);
void screen_wifi_disconnect(void);
void screen_wifi_exit(void);
void screen_wifi_init(void);

#endif
