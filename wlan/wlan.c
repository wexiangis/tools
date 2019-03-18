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

//========== wlan维护 ==========

#define SHELL_WIFI_START \
"#!/bin/sh\n"\
"#insmod nl80211\n"\
"if ps | grep -v grep | grep wpa_supplicant | grep wlan0 > /dev/null\n"\
"then\n"\
"    echo \"wpa_supplicant is already running\"\n"\
"else\n"\
"   if [ ! -f /etc/wpa_supplicant.conf ] ; then\n"\
"       echo \"ctrl_interface=/var/run/wpa_supplicant\" > /etc/wpa_supplicant.conf\n"\
"   fi\n"\
"    wpa_supplicant -iwlan0 -Dnl80211 -c/etc/wpa_supplicant.conf -B\n"\
"fi\n"\
"if ps | grep -v grep | grep udhcpc > /dev/null\n"\
"then\n"\
"    echo \"udhcpc is already running\"\n"\
"else\n"\
"    udhcpc -i wlan0 > /dev/null &\n"\
"fi"

#define SHELL_WIFI_STOP     \
"#!/bin/sh\n"\
"killall udhcpc\n"\
"wpa_cli -iwlan0 terminate\n"\
"ip link set wlan0 down"

#define CMD_WPA_CLI       "wpa_cli -i wlan0"

#define  WPA_CLI_CMD_LEN    512
#define  WPA_CLI_RESULT_LEN    10240

typedef struct{
    //----- wifi -----
    //使用管道与wifi配置小工具 wpa_cli 保持交互
    DuplexPipe wpa_cli;
    //指令发/收
    char cmd[WPA_CLI_CMD_LEN];
    char cmdReady;//0/空闲 1/在编辑 2/允许发出
    char cmdResult[WPA_CLI_RESULT_LEN];//接收缓冲区
    char *cmdResultPoint;//在缓冲区中游走的指针
    int cmdResultReady;//0/空闲 1/等待结果写入 >1/正在写入 数值停止增长/续传结束,可以开始解析
    //扫描
    bool scan;//在扫描中
    int scanTimeout;
    //扫描回调
    void *scanObject;
    ScanCallback scanCallback;
    unsigned int wifi_rate;//网速 bytes/s
    //----- ap -----

    //当前连接信息
    Wlan_Status status;
}Wlan_Struct;

static Wlan_Struct *wlan = NULL;

//发指令
bool _wpa_cli_cmd(Wlan_Struct *wlan, char *cmd, ...)
{
    if(wlan && wlan->wpa_cli.run && cmd)
    {
		char buff[WPA_CLI_CMD_LEN] = {0};
		va_list ap;
		va_start(ap , cmd);
		vsnprintf(buff, WPA_CLI_CMD_LEN, cmd, ap);
		va_end(ap);
        //发指令
        int i = 1;
        while(wlan->cmdReady)//等待空闲
        {
            wlan_delay_ms(100);
            if(!wlan->wpa_cli.run || ++i > 30)//3秒超时
                return false;
        }
        wlan->cmdReady = 1;//在编辑
        memcpy(wlan->cmd, buff, WPA_CLI_CMD_LEN);
        wlan->cmdReady = 2;//发出
        return true;
    }
    return false;
}

//发指令,并等待期望返回 失败返回 NULL 成功返回 wlan->cmdResult 指针
char *_wpa_cli_cmd2(Wlan_Struct *wlan, char *expect, char *cmd, ...)
{
    int i, j, syncFlag;
    //
    if(wlan && wlan->wpa_cli.run)
    {
        i = 0;
        while(wlan->cmdResultReady)//等待空闲
        {
            wlan_delay_ms(100);
            if(!wlan->wpa_cli.run || ++i > 30)//3秒超时
                return NULL;
        }
        wlan->cmdResultReady = -1;//占用
        //发出指令
		char buff[WPA_CLI_CMD_LEN] = {0};
		va_list ap;
		va_start(ap , cmd);
		vsnprintf(buff, WPA_CLI_CMD_LEN, cmd, ap);
		va_end(ap);
        i = 1;
        while(wlan->cmdReady)//等待空闲
        {
            wlan_delay_ms(100);
            if(!wlan->wpa_cli.run || ++i > 30)//3秒超时
            {
                wlan->cmdResultReady = 0;//解占
                return NULL;
            }
        }
        wlan->cmdReady = 1;//在编辑
        memcpy(wlan->cmd, buff, WPA_CLI_CMD_LEN);
        wlan->cmdReady = 2;//发出
        //等待回复
        wlan->cmdResultReady = syncFlag = 1;
        for(i = j = 0; i < 30; i++)//3秒超时
        {
            wlan_delay_ms(100);
            if(!wlan->wpa_cli.run || ++j > 60)//6秒超时
            {
                wlan->cmdResultReady = 0;//解占
                return NULL;
            }
            else if(syncFlag != wlan->cmdResultReady)//正在续传
            {
                syncFlag = wlan->cmdResultReady;
                i = 25;//半秒倒计时
            }
        }
        //匹配回复
        if(!expect || (expect && strstr(wlan->cmdResult, expect)))
        {
            wlan->cmdResultReady = 0;//解占
            return wlan->cmdResult;
        }
        else
        {
            wlan->cmdResultReady = 0;//解占
            return NULL;
        }
    }
    return NULL;
}

void _wpa_scan_info_release(WlanScan_Info *info)
{
    WlanScan_Info *in = info, *inNext;
    while(in)
    {
        inNext = in->next;
        free(in);
        in = inNext;
    }
}

WlanScan_Info *_wpa_scan_info(WlanScan_Info *info, char *str, int *num)
{
    WlanScan_Info *in = info, *next, tInfo;
    char *p, keyType[512];
    int i, total = 0;
    if(str)
    {
        if((p = strstr(str, "scan_result")))
        {
            //跳过两行
            for(i = 0; *p && i < 2;){
                if(*p++ == '\n')
                    i += 1;
            }
            //逐行解析
            while(*p)
            {
                if((*p >= '0' && *p <= '9') ||
                    (*p >= 'a' && *p <= 'z') ||
                    (*p >= 'A' && *p <= 'Z'))
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

void _thr_wpa_cli_scan(Wlan_Struct *wlan)
{
    char *ret;
    int num = 0;
    WlanScan_Info *info = NULL;
    //
    wlan->scan = true;
    if(_wpa_cli_cmd2(wlan, NULL, "scan\n"))//开始扫描
    {
        while(wlan->scan)
        {
            //延时
            wlan_delay_ms(1000);
            //发指令
            if((ret = _wpa_cli_cmd2(wlan, ">", "scan_result\n")))
            {
                if(wlan->scanCallback && (info = _wpa_scan_info(info, ret, &num)))
                {
                    wlan->scanCallback(wlan->scanObject, num, info);
                    _wpa_scan_info_release(info);
                    info = NULL;
                }
            }
            //倒计时
            if(--wlan->scanTimeout < 1)
                wlan->scan = false;
        }
    }
}

//argv/传给回调函数的用户结构体 
//callback/回调函数
//timeout/扫描时长,超时后不再扫描,不再回调 建议值7秒
void wifi_scan(void *object, ScanCallback callback, int timeout)
{
    if(wlan && wlan->wpa_cli.run)
    {
        wlan->scanObject = object;
        wlan->scanCallback = callback;
        wlan->scanTimeout = 10;//timeout;
        if(!wlan->scan)//当前没有在扫描
        {
            //开线程
            pthread_attr_t attr;
            pthread_t th;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);//禁用线程同步, 线程运行结束后自动释放
            if(pthread_create(&th, &attr, (void *)&_thr_wpa_cli_scan, (void *)wlan) < 0)
                wlan->scanTimeout = 0;//scan=false scanTimeout=0 
            //attr destroy
            pthread_attr_destroy(&attr);
        }
    }
}

int wifi_connect(char *ssid, char *key)
{
    if(!wlan || !wlan->wpa_cli.run)
        return -1;

    //关闭扫描
    wlan->scan = false;

    //添加网络
    if(!_wpa_cli_cmd2(wlan, ">", "add_network\n"))
    {
        sprintf(stderr, "[add_network err !]\n");
        return -1;
    }
    else
	    sscanf(wlan->cmdResult, "%*[^0-9]%d", &wlan->status.id);
    //设置名称
    if(!_wpa_cli_cmd2(wlan, "OK", "set_network %d ssid \"%s\"\n", wlan->status.id, ssid))
    {
        sprintf(stderr, "[set_network %d ssid \"%s\" err !]\n", wlan->status.id, ssid);
        return -1;
    }
    //
    if(key)
    {
        //设置密码
        if(!_wpa_cli_cmd2(wlan, "OK", "set_network %d psk \"%s\"\n", wlan->status.id, key))
        {
            sprintf(stderr, "[set_network %d psk \"%s\" err !]\n", wlan->status.id, key);
            return -1;
        }
        //设置密码类型
        if(!_wpa_cli_cmd2(wlan, "OK", "set_network %d key_mgmt WPA-PSK\n", wlan->status.id))
        {
            sprintf(stderr, "[set_network %d key_mgmt WPA-PSK err !]\n", wlan->status.id);
            return -1;
        }
    }
    else
    {
        //设置密码类型
        if(!_wpa_cli_cmd2(wlan, "OK", "set_network %d key_mgmt NONE\n", wlan->status.id))
        {
            sprintf(stderr, "[set_network %d key_mgmt NONE err !]\n", wlan->status.id);
            return -1;
        }
    }
    //启动连接
    if(!_wpa_cli_cmd2(wlan, "OK", "enable_network %d\n", wlan->status.id))
    {
        sprintf(stderr, "[enable_network %d err !]\n", wlan->status.id);
        return -1;
    }
    //start : wpa_supplicant, udhcpc
    // execl("/bin/sh", "sh", "-c", SHELL_WIFI_START, (char *)0);
    system(SHELL_WIFI_START);
    //
    return wlan->status.id;
}

int wifi_disconnect(void)
{
    if(!wlan || !wlan->wpa_cli.run)
        return -1;

    //关闭扫描
    wlan->scan = false;

    //移除当前连接
    if(!_wpa_cli_cmd(wlan, "remove_network %d\n", wlan->status.id))
    {
        sprintf(stderr, "[remove_network %d err !]\n", wlan->status.id);
        return -1;
    }
    return wlan->status.id;
}

//
Wlan_Status *wifi_status(void)
{
    char *ret, *p;

    if(!wlan || !wlan->wpa_cli.run)
        return NULL;
    
    if(!(ret = _wpa_cli_cmd2(wlan, ">", "status\n")))
    {
        sprintf(stderr, "[status err !]\n");
        return NULL;
    }

	//bssid
    memset(wlan->status.bssid, 0, sizeof(wlan->status.bssid));
    if((p = strstr(ret, "bssid=")))
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
    if((p = strstr(ret, "ip_address=")))
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
    if((p = strstr(ret, "p2p_device_address=")))
        sscanf(p+19, "%s", wlan->status.p2p_dev_addr);
    //uuid
    memset(wlan->status.uuid, 0, sizeof(wlan->status.uuid));
    if((p = strstr(ret, "uuid=")))
        sscanf(p+5, "%s", wlan->status.uuid);
    //frq
    if((p = strstr(ret, "freq=")))
        sscanf(p+5, "%d", &wlan->status.frq);
    //keyType
    if((p = strstr(ret, "key_mgmt=")))
    {
        p += 9;
        if(strncmp(p, "NONE", 4) == 0)
            wlan->status.keyType = 0;
        else
            wlan->status.keyType = 1;
    }
    //status
    if((p = strstr(ret, "wpa_state=")))
    {
        p += 10;
        if(strncmp(p, "COMPLETED", 9) == 0)
            wlan->status.status = 1;
        else
            wlan->status.status = 0;
    }else
        wlan->status.status = 0;
    //
    return &wlan->status;
}

//指令透传
char *wifi_through(char *cmd)
{
    if(!wlan || !wlan->wpa_cli.run)
        return NULL;
    return _wpa_cli_cmd2(wlan, NULL, "%s\n", cmd);
}

void _thr_wpa_cli_read(Wlan_Struct *wlan)
{
    char buff[1024];
    int ret;
    while(wlan->wpa_cli.run)
    {
        memset(buff, 0, 1024);
        ret = read(wlan->wpa_cli.fr, buff, 1024);//此处为阻塞读
        if(ret > 0)
        {
            if(wlan->cmdResultReady > 0)//允许解析指令
            {
                if(wlan->cmdResultReady == 1)//开始保存返回信息
                {
                    memset(wlan->cmdResult, 0, WPA_CLI_RESULT_LEN);//缓冲区清空
                    wlan->cmdResultPoint = wlan->cmdResult;//重置指针
                    memcpy(wlan->cmdResultPoint, buff, ret);//拷贝
                }
                else//续传返回信息
                {
                    //继续拷贝是否会超出缓冲区?
                    if(wlan->cmdResultPoint - wlan->cmdResult <= WPA_CLI_RESULT_LEN - ret)
                        memcpy(wlan->cmdResultPoint, buff, ret);//拷贝
                }
                //指针移动
                wlan->cmdResultPoint += ret;
                //续传计数+1
                wlan->cmdResultReady += 1;
            }
            else//其它冷不丁的信息
            {
                ;
            }
            //
            // printf("%s\n", buff);
            continue;
        }
        else//检查子进程状态
        {
            if(waitpid(wlan->wpa_cli.pid, NULL, WNOHANG|WUNTRACED) != 0)
            {
                wlan->wpa_cli.run = false;
                wlan_delay_ms(200);
                duplex_pclose(&wlan->wpa_cli);
                break;
            }
        }
        wlan_delay_ms(100);
    }
    printf("wpa_cli_read exit ...\n");
}

void _thr_wpa_cli_write(Wlan_Struct *wlan)
{
    int ret;
    while(wlan->wpa_cli.run)
    {
        if(wlan->cmdReady == 2)
        {
            ret = write(wlan->wpa_cli.fw, wlan->cmd, strlen(wlan->cmd));
            wlan->cmdReady = 0;
            if(ret < 1)//检查子进程状态
            {
                if(waitpid(wlan->wpa_cli.pid, NULL, WNOHANG|WUNTRACED) != 0)
                {
                    wlan->wpa_cli.run = false;
                    wlan_delay_ms(200);
                    duplex_pclose(&wlan->wpa_cli);
                    break;
                }
            }
        }
        wlan_delay_ms(200);
    }
    printf("wpa_cli_write exit ...\n");
}

void wifi_exit(void)
{
    //kill : udhcpc, wpa_supplicant
    // execl("/bin/sh", "sh", "-c", SHELL_WIFI_STOP, (char *)0);
    system(SHELL_WIFI_STOP);

    //kill : wpa_cli
    if(wlan)
        duplex_pclose(&wlan->wpa_cli);
}

void wifi_init(void)
{
    pthread_attr_t attr;
    pthread_t th_r, th_w;

    //start : wpa_supplicant, udhcpc
    // execl("/bin/sh", "sh", "-c", SHELL_WIFI_START, (char *)0);
    system(SHELL_WIFI_START);

    //start : wpa_cli
    if(wlan == NULL)
        wlan = (Wlan_Struct *)calloc(1, sizeof(Wlan_Struct));
    if(!wlan->wpa_cli.run) //already running ?
        duplex_popen(&wlan->wpa_cli, CMD_WPA_CLI);
    
    //attr init
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);//禁用线程同步, 线程运行结束后自动释放
    //开启维护线程
    pthread_create(&th_w, &attr, (void *)&_thr_wpa_cli_write, (void *)wlan);
    pthread_create(&th_r, &attr, (void *)&_thr_wpa_cli_read, (void *)wlan);
    //attr destroy
    pthread_attr_destroy(&attr);
}
