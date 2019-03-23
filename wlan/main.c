
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "wlan.h"

#define TEST_MODE 2

#if(TEST_MODE == 2)

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
#define CMD_LEN 128
#define RSP_LEN 10240
    char cmd[CMD_LEN], rsp[RSP_LEN];
    int ret;
    //
    wifi_init();
    //
    while(1)
    {
        ret = 0;
        memset(cmd, 0, CMD_LEN);
        if(fgets(cmd, CMD_LEN, stdin))
        {
            if(cmd[0] == 'Q' || cmd[0] == 'q')
                break;
            else if(cmd[0] == '1')
                wifi_scan(NULL, (ScanCallback)&wifi_scanCallback, 3);
            else if(cmd[0] == '2')
                wifi_scanStop();
            else if(cmd[0] == '3')
                ret = wlan_request("STATUS", 6, rsp, RSP_LEN);
            else if(cmd[0] == '4')
                ret = wlan_request("LIST_NETWORKS", 13, rsp, RSP_LEN);
            else if(cmd[0] == '5')
                ret = wlan_request("SIGNAL_POLL", 11, rsp, RSP_LEN);
            else if(cmd[0] == '9')
                ret = wlan_request("SAVE_CONFIG", 11, rsp, RSP_LEN);
            else if(cmd[0] == ':')
                ret = wlan_request(&cmd[1], strlen(cmd)-2, rsp, RSP_LEN);//strlen(cmd)-2是去掉回车
            else if(cmd[0] == 'c' || cmd[0] == 'C')
                wifi_connect("AP-QBOX10", "12345678");
            else if(cmd[0] == 'd' || cmd[0] == 'D')
                wifi_disconnect();
            else if(cmd[0] == 's' || cmd[0] == 'S')
                printf("<signal : %ddbm>\n", wifi_signal());
            else if(cmd[0] == '0')
                break;
            //
            if(ret > 0)
            {
                rsp[ret] = 0;
                printf("<wlan_request: %d>\n%s\n", ret, rsp);
            }
        }
    }
    //
    wifi_exit();
    //
    return 0;
}

#elif(TEST_MODE == 1)

#include "wpa_ctrl.h"

#define PATH_WLAN0 "/var/run/wpa_supplicant/wlan0"
#define PATH_P2P_WLAN0 "/var/run/wpa_supplicant/p2p-dev-wlan0"

#define CMD_LEN 128
#define RSP_LEN 10240

void callback(char *msg, size_t len)
{
    printf("callback: %d\n%s\n", len, msg);
}

void wpa_ctrl_thr(struct wpa_ctrl *ctrl)
{
    char rsp[RSP_LEN];
    int ret, rspLen;
    //
    if(wpa_ctrl_attach(ctrl) == 0)
    {
        while(1)
        {
            memset(rsp, 0, RSP_LEN);
            rspLen = RSP_LEN;
            ret = wpa_ctrl_recv(ctrl, rsp, &rspLen);
            if(ret == 0 && rspLen > 0)
                printf("recv: %d\n%s\n", rspLen, rsp);
            else 
                ;//printf("recv: %d\n", ret);
            sleep(1);
        }
    }
    //
    wpa_ctrl_detach(ctrl);
    //
    printf("wpa_ctrl_thr exit ...\n");
}

int main(void)
{
    char cmd[CMD_LEN], rsp[RSP_LEN];
    int cmdLen, rspLen;
    struct wpa_ctrl *ctrl[2] = {0}, *ctrl_p2p[2] = {0}, *cl;

    //init
    if((ctrl[0] = wpa_ctrl_open(PATH_WLAN0)) == NULL)
    {
        fprintf(stderr, "open %s err\n", PATH_WLAN0);
        return -1;
    }
    if((ctrl[1] = wpa_ctrl_open(PATH_WLAN0)) == NULL)
    {
        fprintf(stderr, "open %s err\n", PATH_WLAN0);
        return -1;
    }
    if((ctrl_p2p[0] = wpa_ctrl_open(PATH_P2P_WLAN0)) == NULL)
    {
        fprintf(stderr, "open %s err\n", PATH_P2P_WLAN0);
        return -1;
    }
    if((ctrl_p2p[1] = wpa_ctrl_open(PATH_P2P_WLAN0)) == NULL)
    {
        fprintf(stderr, "open %s err\n", PATH_P2P_WLAN0);
        return -1;
    }

    //
    pthread_t th1, th2;
    pthread_create(&th1, NULL, (void*)&wpa_ctrl_thr, (void*)ctrl[1]);
    pthread_create(&th2, NULL, (void*)&wpa_ctrl_thr, (void*)ctrl_p2p[1]);
    
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
            }else if(cmd[0] == '1')
                strcpy(cmd, "SCAN\n");
            else if(cmd[0] == '2')
                strcpy(cmd, "SCAN_RESULTS\n");
            else if(cmd[0] == '3')
                strcpy(cmd, "STATUS\n");
            //
            cmdLen = strlen(cmd) - 1;
            cmd[cmdLen] = 0;
            memset(rsp, 0, RSP_LEN);
            rspLen = RSP_LEN;
            //
            if(cmdLen == 1 && cmd[0] == 'Q')
            {
                wpa_ctrl_close(ctrl_p2p[0]);
                wpa_ctrl_close(ctrl[0]);
                return 0;
            }
            else if(strstr(cmd, "P2P"))
                cl = ctrl_p2p[0];
            else
                cl = ctrl[0];
            //
            if(wpa_ctrl_request(cl, cmd, cmdLen, rsp, &rspLen, (void*)&callback) == 0)
            // if(wpa_ctrl_request(cl, cmd, cmdLen, rsp, &rspLen, NULL) == 0)
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

#elif(TEST_MODE == 0)

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
