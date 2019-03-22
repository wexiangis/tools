#ifndef __WLAN_H_
#define __WLAN_H_

#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>

#define WLAN_MODE 1

//----- 全双工管道封装 -----

typedef struct{
	int fr, fw;
	pid_t pid;
	bool run;
}DuplexPipe;

bool duplex_popen(DuplexPipe *dp, char *cmd);
void duplex_pclose(DuplexPipe *dp);

#if(WLAN_MODE == 1)

#include "wpa_ctrl.h"



#else

//----- wlan -----

typedef struct WlanScanInfo{
	char bssid[32];
	int frq;
    int power;
    int keyType;
    char ssid[256];
    struct WlanScanInfo *next;
}WlanScan_Info;

//wifi 扫描每新增一条网络就回调该函数,由用户自行处理新增的网络
//object/用户数据指针
//example:
//	void wifi_scanCallback(void *object, int num, WlanScan_Info *info);
typedef void (*ScanCallback)(void *object, int num, WlanScan_Info *info);

typedef struct{
	char bssid[32];
	int frq;
    // int power;
    int keyType;
    char ssid[256];
	int id;
	char ip[32];
	char addr[32];
	char p2p_dev_addr[32];
	char uuid[128];
	char status;//0/关闭 1/开启
}Wlan_Status;

void wifi_scan(void *object, ScanCallback callback, int timeout);
void wifi_scanStop(void);
int wifi_connect(char *ssid, char *key);
int wifi_disconnect(void);
Wlan_Status *wifi_status(void);
char *wifi_through(char *cmd);//指令透传
void wifi_exit(void);
void wifi_init(void);

// network_dev : 为热点提供上网源的网络设备,例如 eth0 ppp0
bool ap_start(char *name, char *key, ScanCallback callback, char *network_dev);
void ap_stop();

#endif

#endif
