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

#define SHELL_WIFI_START  "/etc/wpa_supplicant/start.sh"
/*
#!/bin/sh

#insmod nl80211

if ps | grep -v grep | grep wpa_supplicant | grep wlan0 > /dev/null
then
    echo "wpa_supplicant is already running"
else
    wpa_supplicant -iwlan0 -Dnl80211 -c/etc/wpa_supplicant/wpa_supplicant.conf -B
fi

if ps | grep -v grep | grep udhcpc > /dev/null
then
    echo "udhcpc is already running"
else
    udhcpc -i wlan0 > /dev/null &
fi
*/

#define SHELL_WIFI_STOP   "/etc/wpa_supplicant/stop.sh"
/*
#!/bin/sh
killall udhcpc
wpa_cli -iwlan0 terminate
*/

#define CMD_WPA_CLI     "wpa_cli -i wlan0"

#define  WPA_CLI_CMD_LEN	1024

//wifi 扫描每新增一条网络就回调该函数,由用户自行处理新增的网络
typedef void (*ScanCallback)(void *object, char *name, int keyType, int power);

typedef struct{
	//----- wifi -----
    //使用管道与wifi配置小工具 wpa_cli 保持交互
    DuplexPipe wpa_cli;
    //指令发/收
    char cmd[WPA_CLI_CMD_LEN];
    char cmdReady;//0/空闲 1/在编辑 2/允许发出
	char cmdResult[WPA_CLI_CMD_LEN];
    char cmdResultReady;//0/空闲 1/等待结果写入 2/发指令者正在解析结果(解析完后务必置0)
    //扫描
	bool scan;//在扫描中
	int scanTimeout;
	void *scanObject;
    ScanCallback scanCallback;
    //当前连接信息
	int wifi_status;//0/无连接 1/在连接 2/获取ip 3/正常
	char wifi_ssid[128];
	char wifi_key[128];
	char wifi_keyType;//0/无 1/WPA-PSK 2/WPA2-PSK
	char wifi_ip[24];
	char wifi_mac[24];
	int wifi_freq;//当前连接网络 频段 0/无 24xx/2.4G 5xxx/5G
	int wifi_power;//当前连接网络 信号强度 dbm
	unsigned int wifi_rate;//网速 bytes/s
	//----- ap -----
}ScreenWlan_Struct;

void screen_wifi_scan(void *object, ScanCallback callback, int timeout);
void screen_wifi_connect(char *ssid, char *key);
void screen_wifi_disconnect(void);
void screen_wifi_exit(void);
void screen_wifi_init(void);

#endif
