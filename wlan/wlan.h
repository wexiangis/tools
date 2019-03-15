#ifndef __WLAN_H_
#define __WLAN_H_

#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>

//----- 全双工管道封装 -----

typedef struct{
	int fr, fw;
	pid_t pid;
	bool run;
}DuplexPipe;

bool duplex_popen(DuplexPipe *dp, const char *cmd);
void duplex_pclose(DuplexPipe *dp);

//----- wlan -----

//wifi 扫描每新增一条网络就回调该函数,由用户自行处理新增的网络
typedef void (*ScanCallback)(void *object, char *name, int keyType, int power);

void wifi_scan(void *object, ScanCallback callback, int timeout);
void wifi_connect(char *ssid, char *key);
void wifi_disconnect(void);
int wifi_status(void);
void wifi_exit(void);
void wifi_init(void);

#endif
