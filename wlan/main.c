
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wlan.h"

#if(WLAN_MODE == 1)

#define PATH_WLAN0 "/var/run/wpa_supplicant/wlan0"
#define PATH_P2P_WLAN0 "/var/run/wpa_supplicant/p2p-dev-wlan0"

#define CMD_LEN 128
#define RSP_LEN 1024

void callback(char *msg, size_t len)
{
    printf("callback: %d\n%s\n", len, msg);
}

int main(void)
{
    char cmd[CMD_LEN], rsp[RSP_LEN];
    int cmdLen, rspLen;
    struct wpa_ctrl *ctrl = NULL, *ctrl_p2p = NULL, *cl;
    
    //init
    if((ctrl = wpa_ctrl_open(PATH_WLAN0)) == NULL)
    {
        fprintf(stderr, "open %s err\n", PATH_WLAN0);
        return -1;
    }
    if((ctrl_p2p = wpa_ctrl_open(PATH_P2P_WLAN0)) == NULL)
    {
        fprintf(stderr, "open %s err\n", PATH_P2P_WLAN0);
        return -1;
    }
    
    //cmd test
    fprintf(stdout, "cmd test begin:\n>");
    while(1)
    {
        memset(cmd, 0, CMD_LEN);
        if(fgets(cmd, CMD_LEN, stdin))
        {
            if(cmd[0] == '\n')
            {
                fputc('>', stdout);
                fflush(stdout);
                continue;
            }
            //
            cmdLen = strlen(cmd) - 1;
            cmd[cmdLen] = 0;
            memset(rsp, 0, RSP_LEN);
            rspLen = RSP_LEN;
            //
            if(strstr(cmd, "P2P"))
                cl = ctrl_p2p;
            else
                cl = ctrl;
            //
            if(wpa_ctrl_request(cl, cmd, cmdLen, rsp, &rspLen, (void*)&callback) == 0)
            {
                fprintf(stdout, "%s>", rsp);
                fflush(stdout);
            }
        }
        //
        sleep(1);
    }
    
    return 0;
}

#elif(WLAN_MODE == 0)

void wifi_scanCallback(void *object, int num, WlanScan_Info *info)
{
    WlanScan_Info *tInfo = info;
    while(tInfo)
    {   
        printf("scan result : %d\n", num);
        printf("  ssid: %s\n", tInfo->ssid);
        printf("  frq: %d\n", tInfo->frq);
        printf("  power: %d\n", tInfo->power);
        printf("  keyType: %d\n", tInfo->keyType);
        printf("  bssid: %s\n\n", tInfo->bssid);
        tInfo = tInfo->next;
    }
}

int main(void)
{
    char scanfStr[128];
	int ret;
    //
    while(1)
    {
        if(scanf("%s", scanfStr) > 0)
        {
            if(scanfStr[0] == 'w')
            {
                if(scanfStr[1] == '1')
                    wifi_init();
                else if(scanfStr[1] == '2')
                    wifi_scan(NULL, &wifi_scanCallback, 5);
                else if(scanfStr[1] == '3')
                {
                    ret = wifi_connect("Guest hi-T", NULL);
                    if(ret < 0)
                        sprintf(stderr, "wifi_connect failed\n");
                    else
                        printf("wifi_connect %d success\n", ret);
                }
                else if(scanfStr[1] == '4')
                {
                    ret = wifi_disconnect();
                    if(ret < 0)
                        sprintf(stderr, "wifi_disconnect failed\n");
                    else
                        printf("wifi_disconnect %d success\n", ret);
                }
                else if(scanfStr[1] == '5')
                {
                    Wlan_Status *status = NULL;
                    if((status = wifi_status()))
                    {
                        printf("status: id = %d, s = %d\n", status->id, status->status);
                        printf("  ssid: %s\n", status->ssid);
                        printf("  frq: %d\n", status->frq);
                        printf("  keyType: %d\n", status->keyType);
                        printf("  ip: %s\n", status->ip);
                        printf("  bssid: %s\n", status->bssid);
                        printf("  addr: %s\n", status->addr);
                        printf("  p2p_dev_addr: %s\n", status->p2p_dev_addr);
                        printf("  uuid: %s\n\n", status->uuid);
                    }
                    else
                        sprintf(stderr, "wifi_status failed\n");
                }
                else if(scanfStr[1] == ':')
                    printf("%s", wifi_through(&scanfStr[2]));
                else if(scanfStr[1] == '0')
                    wifi_exit();
                else if(scanfStr[1] == 'c' && scanfStr[2] == ':')
                {
                    ret=2;
                    while(scanfStr[++ret])
                    {
                        if(scanfStr[ret] == ':')
                        {
                            scanfStr[ret++] = 0;
                            ret = wifi_connect(&scanfStr[3], (scanfStr[ret]?(&scanfStr[ret]):NULL));
                            if(ret < 0)
                                sprintf(stderr, "wifi_connect failed\n");
                            else
                                printf("wifi_connect %d success\n", ret);
                        }
                    }
                }
            }
            else if(scanfStr[0] == 'a')
            {
                if(scanfStr[1] == ':')
                {
                    char name[64] = {0}, key[32] = {0}, dev[32] = {0};
                    if(sscanf(&scanfStr[2], "%[^:]%*[:]%[^:]%*[:]%[^:]", 
                        name, key, dev) == 3)
                    {
                        if(ap_start(name, key, NULL, dev))
                            printf("ap %s : %s / %s success\n", dev, name, key);
                        else
                            printf("ap %s : %s / %s failed\n", dev, name, key);
                    }
                }
                else if(scanfStr[1] == 's')
                    ap_stop();
            }
            //
            scanfStr[0] = 0;
        }
    }
	return 0;	
}
#endif
