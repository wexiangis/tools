
#include <stdio.h>
#include <stdlib.h>
#include "wlan.h"

void wifi_scanCallback(void *object, int num, WlanScan_Info *info)
{
    WlanScan_Info *tInfo = info;
    while(tInfo)
    {
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
            //
            scanfStr[0] = 0;
        }
    }
	return 0;	
}

