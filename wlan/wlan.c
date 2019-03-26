#include "wlan.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

///
void wlan_delay_ms(unsigned int ms)
{
    struct timeval delay;
    wpa_ctrl_open(NULL);
    if (ms > 1000)
    {
        delay.tv_sec = ms / 1000;
        delay.tv_usec = (ms % 1000)*1000;
    }
    else
    {
        delay.tv_sec = 0;
        delay.tv_usec = ms*1000;
    }
    select(0, NULL, NULL, NULL, &delay);
}

//========== 全双工管道封装 ==========

bool duplex_popen(DuplexPipe *dp, char *cmd)
{
    int fr[2], fw[2];// read, write
    pid_t pid;

    if(!dp || !cmd)
        return false;

    if(pipe(fr) < 0) return false;
    if(pipe(fw) < 0) return false;

    if((pid = fork()) < 0)
        return false;
    else if(pid == 0) //child process
    {
        close(fr[0]);
        close(fw[1]);
        if(dup2(fr[1], STDOUT_FILENO) != STDOUT_FILENO || 
            dup2(fw[0], STDIN_FILENO) != STDIN_FILENO) //数据流重定向
            fprintf(stderr, "dup2 err !");
        close(fr[1]);
        close(fw[0]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);//最后那个0是用来告诉系统传参结束的
        _exit(127);
    }

    close(fr[1]);
    dp->fr = fr[0];
    close(fw[0]);
    dp->fw = fw[1];

    dp->pid = pid;
    dp->run = true;

    // printf("duplex_popen : %d\n", dp->pid);

    return true;
}

void duplex_pclose(DuplexPipe *dp)
{
    if(dp && dp->pid)// && dp->run)
    {
        dp->run = false;
        if(waitpid(dp->pid, NULL, WNOHANG|WUNTRACED) == 0)
            kill(dp->pid, SIGKILL);
        close(dp->fr); dp->fr = 0;
        close(dp->fw); dp->fw = 0;

        // printf("duplex_pclose : %d\n", dp->pid);

        dp->pid = 0;
    }
}

//========== wlan ==========

#define SHELL_WPA_SUPPLICANT_START \
"#!/bin/sh\n"\
"if ps | grep -v grep | grep wpa_supplicant | grep wlan0 > /dev/null\n"\
"then\n"\
"    echo \"wpa_supplicant is already running\"\n"\
"else\n"\
"   if [ ! -f /etc/wpa_supplicant.conf ] ; then\n"\
"       echo \"ctrl_interface=/var/run/wpa_supplicant\" > /etc/wpa_supplicant.conf\n"\
"       echo \"update_config=1\" >> /etc/wpa_supplicant.conf\n"\
"   fi\n"\
"    wpa_supplicant -iwlan0 -Dnl80211 -c/etc/wpa_supplicant.conf -B\n"\
"fi\n"

/*"wpa_cli -iwlan0 terminate\n"\*/
#define SHELL_WPA_SUPPLICANT_STOP \
"#!/bin/sh\n"\
"killall wpa_supplicant\n"\
"ip link set wlan0 down\n"

#define SHELL_UDHCPC_START \
"#!/bin/sh\n"\
"if ps | grep -v grep | grep udhcpc > /dev/null\n"\
"then\n"\
"    echo \"udhcpc is already running\"\n"\
"else\n"\
"    udhcpc -i wlan0 > /dev/null &\n"\
"fi\n"

#define SHELL_UDHCPC_STOP \
"#!/bin/sh\n"\
"killall udhcpc\n"\

#define RSP_LEN 10240

#define PATH_WLAN0 "/var/run/wpa_supplicant/wlan0"
#define PATH_P2P_WLAN0 "/var/run/wpa_supplicant/p2p-dev-wlan0"

typedef struct{
    //建立对 wpa_supplicant 通信的结构体
    //[0] 用于指令发/收
    //[1] 用于 wpa_supplicant 上报事件接收
    struct wpa_ctrl *ctrl[2]; // wpa_ctrl_open("/var/run/wpa_supplicant/wlan0"); 获得
    struct wpa_ctrl *ctrl_p2p[2]; // wpa_ctrl_open("/var/run/wpa_supplicant/p2p-dev-wlan0"); 获得
    bool run;//用于告知各线程,工作是否继续

    //----- wifi -----
    bool scan;//在扫描
    int timeout;//扫描超时 -1/表示一直扫描
    void *object;
    ScanCallback callback;

    //----- ap -----
    struct wpa_ctrl *ctrl_ap[2]; // wpa_ctrl_open("/var/run/hostapd/wlan0"); 获得
    bool ap_run;

    //----- p2p -----

    //
    Wlan_Status status;
}Wlan_Struct;

static Wlan_Struct *wlan = NULL;

//cmd request callback
void _wlan_callback(char *msg, size_t len)
{
    printf("_wlan_callback: %d\n%s\n", len, msg);
}

//返回回复数据的长度
int wlan_request(struct wpa_ctrl *ctrl, char *cmd, int cmdLen, char *rsp, size_t rspLen)
{
    if(!wlan || !ctrl || !cmd || !rsp || !rspLen)
        return 0;
    // printf(">%s\n", cmd);
    size_t retLen = rspLen;
    if(wpa_ctrl_request(ctrl, cmd, cmdLen, rsp, &retLen, (void*)&_wlan_callback) == 0)
    {
        // printf("%s\n", rsp);
        if(retLen)
            return retLen;
        else
            return 0;
    }
    //
    return 0;
}

//========== wifi & p2p ==========

#define WIFI_ASSERT()  ((wlan && wlan->run)?true:false)

//
void _wlan_recv_thr(struct wpa_ctrl *ctrl)
{
    if(!WIFI_ASSERT() || !ctrl)
        return;
    //
    char rsp[RSP_LEN];
    size_t rspLen;
    int ret;
    //注册监听
    if(wpa_ctrl_attach(ctrl) == 0)
    {
        while(wlan->run)
        {
            memset(rsp, 0, RSP_LEN);
            rspLen = RSP_LEN;//要告知对方 rsp 可用范围
            ret = wpa_ctrl_recv(ctrl, rsp, &rspLen);
            if(ret == 0 && rspLen > 0)
                printf("recv: %d\n%s\n", rspLen, rsp);
            wlan_delay_ms(200);
        }
        //注销监听
        wpa_ctrl_detach(ctrl);
    }
    //
    printf("_wlan_recv_thr exit ...\n");
}

int wifi_through(char *rsp, size_t rspLen, char *cmd, ...)
{
    if(!WIFI_ASSERT() || !cmd || !rsp || !rspLen)
        return 0;
    //
    char buff[1024] = {0};
    va_list ap;
    va_start(ap , cmd);
    vsnprintf(buff, 1024, cmd, ap);
    va_end(ap);
    //
    return wlan_request(wlan->ctrl[0], buff, strlen(buff), rsp, rspLen);
}

int wifi_p2p_through(char *rsp, size_t rspLen, char *cmd, ...)
{
    if(!WIFI_ASSERT() || !cmd || !rsp || !rspLen)
        return 0;
    //
    char buff[1024] = {0};
    va_list ap;
    va_start(ap , cmd);
    vsnprintf(buff, 1024, cmd, ap);
    va_end(ap);
    //
    return wlan_request(wlan->ctrl_p2p[0], buff, strlen(buff), rsp, rspLen);
}

void _wifi_scan_info_release(WlanScan_Info *info)
{
    WlanScan_Info *in = info, *inNext;
    while(in)
    {
        inNext = in->next;
        free(in);
        in = inNext;
    }
}

WlanScan_Info *_wifi_scan_info(WlanScan_Info *info, char *str, int *num)
{
    WlanScan_Info *in = info, *next = NULL, tInfo;
    char *p, keyType[512];
    int i, total = 0;
    if(str)
    {
        if((p = strstr(str, "bssid")))
        {
            //跳过1行
            for(i = 0; *p && i < 1;){
                if(*p++ == '\n')
                    i += 1;
            }
            //逐行解析
            while(*p)
            {
                if((*p >= '0' && *p <= '9') ||
                    (*p >= 'a' && *p <= 'f') ||
                    (*p >= 'A' && *p <= 'F'))
                {
                    memset(&tInfo, 0, sizeof(WlanScan_Info));
                    memset(keyType, 0, 512);
                    if(sscanf(p, "%s %d %d %s %[ -~]", 
                        tInfo.bssid, &tInfo.frq, &tInfo.power, keyType, tInfo.ssid) == 5)
                    {
                        total += 1;
                        //
                        if(strncmp(keyType, "[ESS]", 5) == 0)
                            tInfo.keyType = 0;
                        else
                            tInfo.keyType = 1;
                        //
                        if(!in)
                            next = in = (WlanScan_Info *)calloc(1, sizeof(WlanScan_Info));
                        else
                            next = next->next = (WlanScan_Info *)calloc(1, sizeof(WlanScan_Info));
                        memcpy(next, &tInfo, sizeof(WlanScan_Info));
                        //
                        // printf("  ssid: %s\n", tInfo.ssid);
                        // printf("  frq: %d\n", tInfo.frq);
                        // printf("  power: %d\n", tInfo.power);
                        // printf("  keyType: %d %s\n", tInfo.keyType, keyType);
                        // printf("  bssid: %s\n\n", tInfo.bssid);
                    }
                }
                //跳过该行
                while(*p && *p++ != '\n');
            }
        }
    }
    //
    if(num)
        *num = total;
    return in;
}

//
void _wifi_scan_thr(void)
{
    if(!WIFI_ASSERT())
        return;
    //
    int count = 0;
    char rsp[RSP_LEN];
    //
    int num = 0;
    WlanScan_Info *info = NULL;
    //
    while(wlan->run && wlan->scan)
    {
        //
        memset(rsp, 0, RSP_LEN);
        if(wlan_request(wlan->ctrl[0], "SCAN_RESULTS", 12, rsp, RSP_LEN) < 1)
            break;
        //
        if(wlan->callback && (info = _wifi_scan_info(info, rsp, &num)))
        {
            wlan->callback(wlan->object, num, info);
            _wifi_scan_info_release(info);
            info = NULL;
        }
        //
        if(--wlan->timeout == 0)
            break;
        //
        wlan_delay_ms(500);
        //
        if(++count > 15)//每15秒重发一次 SCAN
        {
            count = 0;
            memset(rsp, 0, RSP_LEN);
            if(wlan_request(wlan->ctrl[0], "SCAN", 4, rsp, RSP_LEN) < 1)
                break;
        }
        //
        wlan_delay_ms(500);
    }
    wlan->scan = false;
}

//
void wifi_scan(void *object, ScanCallback callback, int timeout)
{
    if(!WIFI_ASSERT())
        return;
    //
    wlan->object = object;
    wlan->callback = callback;
    wlan->timeout = timeout;
    //
    char rsp[128];
    if(wlan_request(wlan->ctrl[0], "SCAN", 4, rsp, sizeof(rsp)) < 1)
        return;
    //
    if(!wlan->scan)
    {
        wlan->scan = true;
        //
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);//禁用线程同步, 线程运行结束后自动释放
        pthread_t th;
        pthread_create(&th, &attr, (void *)&_wifi_scan_thr, NULL);
        pthread_attr_destroy(&attr);
    }
}

//
void wifi_scanStop(void)
{
    if(!WIFI_ASSERT())
        return;
    //
    wlan->scan = false;
}

//比较连接历史,返回id号
int _wifi_connect_history_fit(char *ssid)
{
    char rsp[RSP_LEN] = {0}, name[128];
    char *p;
    int id, len = strlen(ssid);
    //
    if(wlan_request(wlan->ctrl[0], "LIST_NETWORKS", 13, rsp, RSP_LEN) > 0)
    {
        p = rsp;
        while((p = strstr(p, "\n")))
        {
            if(*(++p) && sscanf(p, "%d %[^\t]", &id, name) == 2)
            {
                if(strlen(name) == len &&
                    strncmp(ssid, name, len) == 0)
                    return id;
            }
        }
    }
    //
    return -1;
}

//
int wifi_connect(char *ssid, char *key)
{
    if(!WIFI_ASSERT())
        return -1;

    //关闭自动ip
    system(SHELL_UDHCPC_STOP);

    char rsp[RSP_LEN];
    int ret;

    //获得id
    int id = _wifi_connect_history_fit(ssid);
    if(id < 0)
    {
        if((ret = wlan_request(wlan->ctrl[0], "ADD_NETWORK", 11, rsp, RSP_LEN)) < 1)
            return -1;
        rsp[ret] = 0;//截断
        sscanf(rsp, "%d", &id);
    }

    //设置名称
    if(wifi_through(rsp, RSP_LEN, "SET_NETWORK %d ssid \"%s\"", id, ssid) < 1 || 
        rsp[0] != 'O' || rsp[1] != 'K')
        goto failed;

    //设置密码
    if(key && key[0])
    {
        if(wifi_through(rsp, RSP_LEN, "SET_NETWORK %d key_mgmt WPA-PSK", id) < 1 || 
            rsp[0] != 'O' || rsp[1] != 'K')
            goto failed;
        if(wifi_through(rsp, RSP_LEN, "SET_NETWORK %d psk \"%s\"", id, key) < 1 || 
            rsp[0] != 'O' || rsp[1] != 'K')
            goto failed;
    }
    else
    {
        if(wifi_through(rsp, RSP_LEN, "SET_NETWORK %d key_mgmt NONE", id) < 1 || 
            rsp[0] != 'O' || rsp[1] != 'K')
            goto failed;
    }

    //启动自动ip
    system(SHELL_UDHCPC_START);
    
    //
    wlan_delay_ms(500);

    //使能网络
    if(wifi_through(rsp, RSP_LEN, "ENABLE_NETWORK %d", id) < 1 || 
        rsp[0] != 'O' || rsp[1] != 'K')
        goto failed;

    //切换网络
    if(wifi_through(rsp, RSP_LEN, "SELECT_NETWORK %d", id) < 1 || 
        rsp[0] != 'O' || rsp[1] != 'K')
        goto failed;

    //
    return id;

failed:
    wifi_through(rsp, RSP_LEN, "REMOVE_NETWORK %d", id);
    return -1;
}

void wifi_disconnect(void)
{
    if(!WIFI_ASSERT())
        return;

    //
    char rsp[128];
    wlan_request(wlan->ctrl[0], "DISCONNECT", 10, rsp, 128);

    //关闭自动ip
    system(SHELL_UDHCPC_STOP);
}

//
Wlan_Status *wifi_status(void)
{
    if(!WIFI_ASSERT())
        return NULL;
    //
    char rsp[RSP_LEN] = {0}, *p;
    //
    if(wlan_request(wlan->ctrl[0], "STATUS", 6, rsp, RSP_LEN) < 1)
        return NULL;
	//bssid
    memset(wlan->status.bssid, 0, sizeof(wlan->status.bssid));
    if((p = strstr(rsp, "bssid=")))
    {
        p += 6;
        sscanf(p, "%s", wlan->status.bssid);
        //ssid
        memset(wlan->status.ssid, 0, sizeof(wlan->status.ssid));
        if((p = strstr(p, "ssid=")))
            sscanf(p+5, "%[ -~]", wlan->status.ssid);
    }
    //ip
    memset(wlan->status.ip, 0, sizeof(wlan->status.ip));
    if((p = strstr(rsp, "ip_address=")))
    {
        p += 11;
        sscanf(p, "%s", wlan->status.ip);
        //addr
        memset(wlan->status.addr, 0, sizeof(wlan->status.addr));
        if((p = strstr(p, "address=")))
            sscanf(p+8, "%s", wlan->status.addr);
    }
    //p2p_dev_addr
    memset(wlan->status.p2p_dev_addr, 0, sizeof(wlan->status.p2p_dev_addr));
    if((p = strstr(rsp, "p2p_device_address=")))
        sscanf(p+19, "%s", wlan->status.p2p_dev_addr);
    //uuid
    memset(wlan->status.uuid, 0, sizeof(wlan->status.uuid));
    if((p = strstr(rsp, "uuid=")))
        sscanf(p+5, "%s", wlan->status.uuid);
    //frq
    if((p = strstr(rsp, "freq=")))
        sscanf(p+5, "%d", &wlan->status.frq);
    //keyType
    if((p = strstr(rsp, "key_mgmt=")))
    {
        p += 9;
        if(strncmp(p, "NONE", 4) == 0)
            wlan->status.keyType = 0;
        else
            wlan->status.keyType = 1;
    }
    //status
    if((p = strstr(rsp, "wpa_state=")))
    {
        p += 10;
        if(strncmp(p, "SCANNING", 8) == 0)
            wlan->status.status = 1;
        else if(strncmp(p, "COMPLETED", 9) == 0)
            wlan->status.status = 2;
        else
            wlan->status.status = 0;
    }else
        wlan->status.status = 0;
    //
    return &wlan->status;
}

//
int wifi_signalPower(void)
{
    if(!WIFI_ASSERT())
        return 0;
    
    char rsp[1024] = {0}, *p;
    int sig = 0;
    //
    if(wlan_request(wlan->ctrl[0], "SIGNAL_POLL", 11, rsp, 1024) < 1)
        return 0;
    //
    if((p = strstr(rsp, "RSSI")))
        sscanf(p+5, "%d", &sig);
    //
    return sig;
}

int wifi_signal(void)
{
    int sig = wifi_signalPower();
    //=
    if(sig > -30) sig = 100;
    else if(sig < -130) sig = 0;
    else sig = sig + 130;
    //
    return sig;
}

//
void wifi_exit(void)
{
    if(wlan)
    {
        if(wlan->run)
        {
            wlan->run = false;

            wlan_delay_ms(200);

            if(wlan->ctrl[0]) wpa_ctrl_close(wlan->ctrl[0]);
            if(wlan->ctrl[1]) wpa_ctrl_close(wlan->ctrl[1]);
            if(wlan->ctrl_p2p[0]) wpa_ctrl_close(wlan->ctrl_p2p[0]);
            if(wlan->ctrl_p2p[1]) wpa_ctrl_close(wlan->ctrl_p2p[1]);

            wlan->ctrl[0] = wlan->ctrl[1] = NULL;
            wlan->ctrl_p2p[0] = wlan->ctrl_p2p[1] = NULL;
        }

        // free(wlan);
        // wlan = NULL;
    }
    //kill : udhcpc, wpa_supplicant
    system(SHELL_WPA_SUPPLICANT_STOP);
    system(SHELL_UDHCPC_STOP);
}

//
void wifi_init(void)
{
    //start : wpa_supplicant
    system(SHELL_WPA_SUPPLICANT_START);
    
    if(!wlan)
        wlan = (Wlan_Struct *)calloc(1, sizeof(Wlan_Struct));
    //
    if(!wlan->ctrl[0])
        wlan->ctrl[0] = wpa_ctrl_open(PATH_WLAN0);
    if(!wlan->ctrl[1])
        wlan->ctrl[1] = wpa_ctrl_open(PATH_WLAN0);
    if(!wlan->ctrl_p2p[0])
        wlan->ctrl_p2p[0] = wpa_ctrl_open(PATH_P2P_WLAN0);
    if(!wlan->ctrl_p2p[1])
        wlan->ctrl_p2p[1] = wpa_ctrl_open(PATH_P2P_WLAN0);
    //
    if(!wlan->run)
    {
        wlan->run = true;
        //
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);//禁用线程同步, 线程运行结束后自动释放
        //开线程 接收 wpa_supplicant 上报事件
        pthread_t th0, th1;
        if(wlan->ctrl[1])
            pthread_create(&th0, &attr, (void *)&_wlan_recv_thr, (void *)wlan->ctrl[1]);
        if(wlan->ctrl_p2p[1])
            pthread_create(&th1, &attr, (void *)&_wlan_recv_thr, (void *)wlan->ctrl_p2p[1]);
        //
        pthread_attr_destroy(&attr);
    }
}

//========== wlan -- ap热点 ==========

#define SHELL_HOSTAPD_CONF_CREATE \
"#!/bin/sh\n"\
"echo \"interface=wlan0\" > /etc/hostapd.conf\n"\
"echo \"driver=nl80211\" >> /etc/hostapd.conf\n"\
"echo \"ssid=%s\" >> /etc/hostapd.conf\n"\
"echo \"hw_mode=g\" >> /etc/hostapd.conf\n"\
"echo \"channel=10\" >> /etc/hostapd.conf\n"\
"echo \"macaddr_acl=0\" >> /etc/hostapd.conf\n"\
"echo \"auth_algs=1\" >> /etc/hostapd.conf\n"\
"echo \"ignore_broadcast_ssid=0\" >> /etc/hostapd.conf\n"\
"echo \"wpa=%d\" >> /etc/hostapd.conf\n"\
"echo \"wpa_passphrase=%s\" >> /etc/hostapd.conf\n"\
"echo \"wpa_key_mgmt=WPA-PSK\" >> /etc/hostapd.conf\n"\
"echo \"wpa_pairwise=TKIP\" >> /etc/hostapd.conf\n"\
"echo \"rsn_pairwise=CCMP\" >> /etc/hostapd.conf\n"\
"echo \"ctrl_interface=/var/run/hostapd\" >> /etc/hostapd.conf\n"

#define SHELL_UDHCPC_CONF_CHECK \
"#!/bin/sh\n"\
"if [ ! -f /etc/udhcpd.conf ] ; then\n"\
"   echo \"interface wlan0\" > /etc/udhcpd.conf\n"\
"   echo \"start 192.168.43.2\" >> /etc/udhcpd.conf\n"\
"   echo \"end 192.168.43.253\" >> /etc/udhcpd.conf\n"\
"   echo \"opt dns 8.8.8.8 8.8.4.4\" >> /etc/udhcpd.conf\n"\
"   echo \"option subnet 255.255.255.0\" >> /etc/udhcpd.conf\n"\
"   echo \"opt router 192.168.43.1\" >> /etc/udhcpd.conf\n"\
"   echo \"option lease 864000\" >> /etc/udhcpd.conf\n"\
"   echo \"option 0x08 01020304\" >> /etc/udhcpd.conf\n"\
"fi\n"

#define SHELL_HOSTAPD_START \
"#!/bin/sh\n"\
"echo 1 > /proc/sys/net/ipv4/ip_forward\n"\
"ifconfig wlan0 192.168.43.1 netmask 255.255.255.0 up\n"\
"hostapd /etc/hostapd.conf -B\n"\
"if ps | grep -v grep | grep udhcpd | grep wlan0 > /dev/null\n"\
"then\n"\
"    echo \"udhcpd is already running\"\n"\
"else\n"\
"   udhcpd /etc/udhcpd.conf\n"\
"fi\n"

#define SHELL_FIREWALL_START \
"#!/bin/sh\n"\
"iptables -t nat -A POSTROUTING -o %s -j MASQUERADE\n"\
"iptables -A FORWARD -i %s -o wlan0 -m state --state RELATED,ESTABLISHED -j ACCEPT\n"\
"iptables -A FORWARD -i wlan0 -o %s -j ACCEPT\n"

#define SHELL_HOSTAPD_STOP \
"killall hostapd\n"\
"ifconfig wlan0 down\n"

#define PATH_AP_WLAN0 "/var/run/hostapd/wlan0"

//
void _wlan_ap_recv_thr(struct wpa_ctrl *ctrl)
{
    if(!wlan || !ctrl)
        return;
    //
    char rsp[RSP_LEN];
    size_t rspLen;
    int ret;
    //注册监听
    if(wpa_ctrl_attach(ctrl) == 0)
    {
        while(wlan->ap_run)
        {
            memset(rsp, 0, RSP_LEN);
            rspLen = RSP_LEN;//要告知对方 rsp 可用范围
            ret = wpa_ctrl_recv(ctrl, rsp, &rspLen);
            if(ret == 0 && rspLen > 0)
                printf("recv: %d\n%s\n", rspLen, rsp);
            wlan_delay_ms(200);
        }
        //注销监听
        wpa_ctrl_detach(ctrl);
    }
    //
    printf("_wlan_ap_recv_thr exit ...\n");
}

int ap_through(char *rsp, size_t rspLen, char *cmd, ...)
{
    if(!wlan || !wlan->ap_run || !cmd || !rsp || !rspLen)
        return 0;
    //
    char buff[1024] = {0};
    va_list ap;
    va_start(ap , cmd);
    vsnprintf(buff, 1024, cmd, ap);
    va_end(ap);
    //
    return wlan_request(wlan->ctrl_ap[0], buff, strlen(buff), rsp, rspLen);
}

//
void ap_list_release(WlanAp_List *list)
{
    WlanAp_List *current = list, *next;
    while(current)
    {
        next = current->next;
        free(current);
        current = next;
    }
}

WlanAp_List *ap_list(int *total)
{
    if(!wlan || !wlan->ap_run)
        return NULL;
    //
    char rsp[RSP_LEN] = {0}, *p;
    int ret, count = 0;
    WlanAp_List *next, *list = NULL;
    //
    if((ret = ap_through(rsp, RSP_LEN, "STA-FIRST")) > 11 && 
        (p = strstr(rsp, "connected_time=")))
    {
        list = next = (WlanAp_List *)calloc(1, sizeof(WlanAp_List));
        sscanf(rsp, "%s", next->addr);
        sscanf(p+15, "%d", &next->time);
        count++;
        memset(rsp, 0, RSP_LEN);
        //
        while(count < 50 && 
            (ret = ap_through(rsp, RSP_LEN, "STA-NEXT %s", next->addr)) > 11 && 
            (p = strstr(rsp, "connected_time=")))
        {
            next = next->next = (WlanAp_List *)calloc(1, sizeof(WlanAp_List));
            rsp[ret] = 0;
            sscanf(rsp, "%s", next->addr);
            sscanf(p+15, "%d", &next->time);
            count++;
            memset(rsp, 0, RSP_LEN);
        }
    }
    //
    if(total)
        *total = count;
    //
    return list;
}

void ap_stop()
{
    if(wlan)
    {
        if(wlan->ap_run)
        {
            wlan->ap_run = false;

            wlan_delay_ms(200);

            if(wlan->ctrl_ap[0]) wpa_ctrl_close(wlan->ctrl_ap[0]);
            if(wlan->ctrl_ap[1]) wpa_ctrl_close(wlan->ctrl_ap[1]);

            wlan->ctrl_ap[0] = wlan->ctrl_ap[1] = NULL;
        }

        system(SHELL_HOSTAPD_STOP);
        wlan->ap_run = false;
        
        // free(wlan);
        // wlan = NULL;
    }
}

bool ap_start(char *name, char *key, char *network_dev)
{
    if(!network_dev || network_dev[0] == 0)
        return false;

    //network_dev 可用检查

    //
    system(SHELL_HOSTAPD_STOP);

    char buff[10240];
    int ret;

    //创建文件 hostapd.conf
    memset(buff, 0, 10240);
    if(key && key[0])
        ret = snprintf(buff, 10240, SHELL_HOSTAPD_CONF_CREATE, name, 2, key);
    else
        ret = snprintf(buff, 10240, SHELL_HOSTAPD_CONF_CREATE, name, 0, "00000000");
    if(ret < 1 || ret >= 10240)
        return false;
    // printf("system: 创建文件 hostapd.conf\n%s\n\n", buff);
    system(buff);

    //检查文件 udhcpd.conf
    // printf("system: 检查文件 udhcpd.conf\n%s\n\n", SHELL_UDHCPC_CONF_CHECK);
    system(SHELL_UDHCPC_CONF_CHECK);

    //启动 hostapd
    // printf("system: 启动 hostapd\n%s\n\n", SHELL_HOSTAPD_START);
    system(SHELL_HOSTAPD_START);

    //网络转发配置
    memset(buff, 0, 10240);
    ret = snprintf(buff, 10240, SHELL_FIREWALL_START, network_dev, network_dev, network_dev);
    // printf("system: 网络转发配置\n%s\n\n", buff);
    system(buff);

    //
    if(wlan == NULL)
        wlan = (Wlan_Struct *)calloc(1, sizeof(Wlan_Struct));
    if(!wlan->ctrl_ap[0])
        wlan->ctrl_ap[0] = wpa_ctrl_open(PATH_AP_WLAN0);
    if(!wlan->ctrl_ap[1])
        wlan->ctrl_ap[1] = wpa_ctrl_open(PATH_AP_WLAN0);
    //
    if(!wlan->ap_run)
    {
        wlan->ap_run = true;
        //
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);//禁用线程同步, 线程运行结束后自动释放
        //开线程 接收 hostapd 上报事件
        pthread_t th0;
        if(wlan->ctrl_ap[1])
            pthread_create(&th0, &attr, (void *)&_wlan_ap_recv_thr, (void *)wlan->ctrl_ap[1]);
        //
        pthread_attr_destroy(&attr);
    }
    //
    return true;
}

