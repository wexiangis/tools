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

bool duplex_popen(DuplexPipe *dp, char *cmd);
void duplex_pclose(DuplexPipe *dp);

//----- wlan -----

//wifi 扫描每新增一条网络就回调该函数,由用户自行处理新增的网络
//object/用户数据指针
//num/扫描得到的网络数目
//**name,*keyType,*power/分别为网络名称,加密类型,信号强度的数组,长度为num
typedef void (*ScanCallback)(void *object, int num, char **name, int *keyType, int *power);

void wifi_scan(void *object, ScanCallback callback, int timeout);
void wifi_connect(char *ssid, char *key);
void wifi_disconnect(void);
int wifi_status(void);
char *wifi_through(char *cmd);//指令透传
void wifi_exit(void);
void wifi_init(void);

#endif
