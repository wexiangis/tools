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
void view_delay_ms(unsigned int ms)
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

bool screen_duplex_popen(DuplexPipe *dp, const char *cmd)
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
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        _exit(127);
    }

    close(fr[1]);
    dp->fr = fr[0];
    close(fw[0]);
    dp->fw = fw[1];

    dp->pid = pid;
    dp->run = true;

    return true;
}

void screen_duplex_pclose(DuplexPipe *dp)
{
    if(dp && dp->run)
    {
        dp->run = false;
        if(waitpid(dp->pid, NULL, WNOHANG|WUNTRACED) == 0)
            kill(dp->pid, SIGKILL);
        close(dp->fr); dp->fr = 0;
        close(dp->fw); dp->fw = 0;
    }
}

//========== wlan维护 ==========

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

#define  WPA_CLI_CMD_LEN    512
#define  WPA_CLI_RESULT_LEN    10240

//wifi 扫描每新增一条网络就回调该函数,由用户自行处理新增的网络
// typedef void (*ScanCallback)(void *object, char *name, int keyType, int power);

typedef struct{
    //----- wifi -----
    //使用管道与wifi配置小工具 wpa_cli 保持交互
    DuplexPipe wpa_cli;
    //指令发/收
    char cmd[WPA_CLI_CMD_LEN];
    char cmdReady;//0/空闲 1/在编辑 2/允许发出
    char cmdResult[WPA_CLI_RESULT_LEN];//接收缓冲区
    char *cmdResultPoint;//在缓冲区中游走的指针
    char cmdResultReady;//0/空闲 1/等待结果写入 2/正在写入 3/结束写入,由发指令者解析结果(解析完后务必置0)
    //扫描
    bool scan;//在扫描中
    int scanTimeout;
    void *scanObject;
    ScanCallback scanCallback;
    //当前连接信息
    int wifi_id;
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

static ScreenWlan_Struct *wlan = NULL;

//发指令
bool _wpa_cli_cmd(ScreenWlan_Struct *wlan, char *cmd, ...)
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
            view_delay_ms(100);
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
char *_wpa_cli_cmd2(ScreenWlan_Struct *wlan, char *expect, char *cmd, ...)
{
    if(wlan && wlan->wpa_cli.run)
    {
        int i = 0;
        while(wlan->cmdResultReady)//等待空闲
        {
            view_delay_ms(100);
            if(!wlan->wpa_cli.run || ++i > 30)//3秒超时
                return NULL;
        }
        //发出指令
		char buff[WPA_CLI_CMD_LEN] = {0};
		va_list ap;
		va_start(ap , cmd);
		vsnprintf(buff, WPA_CLI_CMD_LEN, cmd, ap);
		va_end(ap);
        i = 1;
        while(wlan->cmdReady)//等待空闲
        {
            view_delay_ms(100);
            if(!wlan->wpa_cli.run || ++i > 30)//3秒超时
                return NULL;
        }
        wlan->cmdReady = 1;//在编辑
        memcpy(wlan->cmd, buff, WPA_CLI_CMD_LEN);
        wlan->cmdReady = 2;//发出
        //等待回复
        wlan->cmdResultReady = 1;
        i = 0;
        while(wlan->cmdResultReady != 2)//等待
        {
            view_delay_ms(100);
            if(!wlan->wpa_cli.run || ++i > 50)//5秒超时
            {
                wlan->cmdResultReady = 0;
                return NULL;
            }
        }
        view_delay_ms(100);
        //匹配回复
        wlan->cmdResultReady = 3;
        if(!expect || (expect && strstr(wlan->cmdResult, expect)))
        {
            wlan->cmdResultReady = 0;
            return wlan->cmdResult;
        }
        else
        {
            wlan->cmdResultReady = 0;
            return NULL;
        }
    }
    return NULL;
}

void _thr_wpa_cli_scan(ScreenWlan_Struct *wlan)
{
    wlan->scan = true;
    _wpa_cli_cmd(wlan, "scan\n");//开始扫描
    while(wlan->scan)
    {
        //发指令
        _wpa_cli_cmd(wlan, "scan_result\n");//刷新结果
        //延时
        view_delay_ms(1000);
        //倒计时
        if(--wlan->scanTimeout < 1)
            wlan->scan = false;
    }
}

//argv/传给回调函数的用户结构体 
//callback/回调函数
//timeout/扫描时长,超时后不再扫描,不再回调 建议值10秒
void screen_wifi_scan(void *object, ScanCallback callback, int timeout)
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

void screen_wifi_connect(char *ssid, char *key)
{
    if(!wlan || !wlan->wpa_cli.run)
        return;

    //关闭扫描
    wlan->scan = false;

    //添加网络
    if(!_wpa_cli_cmd2(wlan, ">", "add_network\n"))
    {
        printf("[add_network err !]\n");
        return;
    }
    else
	    sscanf(wlan->cmdResult, "%*[^0-9]%d", &wlan->wifi_id);
    //设置名称
    if(!_wpa_cli_cmd2(wlan, "OK", "set_network %d ssid \"%s\"\n", wlan->wifi_id, ssid))
    {
        printf("[set_network %d ssid \"%s\" err !]\n", wlan->wifi_id, ssid);
        return;
    }
    //
    if(key)
    {
        //设置密码
        if(!_wpa_cli_cmd2(wlan, "OK", "set_network %d psk \"%s\"\n", wlan->wifi_id, key))
        {
            printf("[set_network %d psk \"%s\" err !]\n", wlan->wifi_id, key);
            return;
        }
        //设置密码类型
        if(!_wpa_cli_cmd2(wlan, "OK", "set_network %d key_mgmt WPA-PSK\n", wlan->wifi_id))
        {
            printf("[set_network %d key_mgmt WPA-PSK err !]\n", wlan->wifi_id);
            return;
        }
    }
    else
    {
        //设置密码类型
        if(!_wpa_cli_cmd2(wlan, "OK", "set_network %d key_mgmt NONE\n", wlan->wifi_id))
        {
            printf("[set_network %d key_mgmt NONE err !]\n", wlan->wifi_id);
            return;
        }
    }
    //启动连接
    if(!_wpa_cli_cmd2(wlan, "OK", "enable_network %d\n", wlan->wifi_id))
    {
        printf("[enable_network %d err !]\n", wlan->wifi_id);
        return;
    }
    //start : wpa_supplicant, udhcpc
    // execl("/bin/sh", "sh", "-c", SHELL_WIFI_START, (char *)0);
    system(SHELL_WIFI_START);
}

void screen_wifi_disconnect(void)
{
    if(!wlan || !wlan->wpa_cli.run)
        return;

    //关闭扫描
    wlan->scan = false;

    //移除当前连接
    if(!_wpa_cli_cmd(wlan, "remove_network %d\n", wlan->wifi_id))
    {
        printf("[remove_network %d err !]\n", wlan->wifi_id);
        return;
    }
}

//
int screen_wifi_status(void)
{
    if(!wlan || !wlan->wpa_cli.run)
        return -1;
    
    _wpa_cli_cmd(wlan, "status\n");

    return 0;
}

void _thr_wpa_cli_read(ScreenWlan_Struct *wlan)
{
    char buff[1024];
    int ret;
    while(wlan->wpa_cli.run)
    {
        memset(buff, 0, 1024);
        ret = read(wlan->wpa_cli.fr, buff, 1024);//此处为阻塞读
        if(ret > 0)
        {
            if(wlan->cmdResultReady == 1)//开始保存返回信息
            {
                //指针和缓冲区初始化
                memset(wlan->cmdResult, 0, WPA_CLI_RESULT_LEN);
                wlan->cmdResultPoint = wlan->cmdResult;
                //拷贝以及指针移动
                memcpy(wlan->cmdResultPoint, buff, ret);
                wlan->cmdResultPoint += ret;
                //标志+1
                wlan->cmdResultReady = 2;
            }
            else if(wlan->cmdResultReady == 2)//续传返回信息
            {
				//继续拷贝是否会超出缓冲区
                if(wlan->cmdResultPoint - wlan->cmdResult <= WPA_CLI_RESULT_LEN - ret)
                {
                	//拷贝以及指针移动
                    memcpy(wlan->cmdResultPoint, buff, ret);
                    wlan->cmdResultPoint += ret;
                }
            }
            else if(wlan->scan)//正在扫描
            {
                //整理数据并回调
                ;
            }
            //
            printf("%s\n", buff);
            //
            continue;
        }
        else//检查子进程状态
        {
            if(waitpid(wlan->wpa_cli.pid, NULL, WNOHANG|WUNTRACED) != 0)
                break;
        }
        view_delay_ms(100);
    }
    printf("wpa_cli_read exit ...\n");
}

void _thr_wpa_cli_write(ScreenWlan_Struct *wlan)
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
                    break;
            }
        }
        view_delay_ms(200);
    }
    printf("wpa_cli_write exit ...\n");
}

void screen_wifi_exit(void)
{
    //kill : udhcpc, wpa_supplicant
    // execl("/bin/sh", "sh", "-c", SHELL_WIFI_STOP, (char *)0);
    system(SHELL_WIFI_STOP);

    //kill : wpa_cli
    if(wlan)
        screen_duplex_pclose(&wlan->wpa_cli);
}

void screen_wifi_init(void)
{
    pthread_attr_t attr;
    pthread_t th_r, th_w;

    //start : wpa_supplicant, udhcpc
    // execl("/bin/sh", "sh", "-c", SHELL_WIFI_START, (char *)0);
    system(SHELL_WIFI_START);

    //start : wpa_cli
    if(wlan == NULL)
        wlan = (ScreenWlan_Struct *)calloc(1, sizeof(ScreenWlan_Struct));
    if(!wlan->wpa_cli.run) //already running ?
        screen_duplex_popen(&wlan->wpa_cli, CMD_WPA_CLI);
    
    //attr init
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);//禁用线程同步, 线程运行结束后自动释放
    //开启维护线程
    pthread_create(&th_w, &attr, (void *)&_thr_wpa_cli_write, (void *)wlan);
    pthread_create(&th_r, &attr, (void *)&_thr_wpa_cli_read, (void *)wlan);
    //attr destroy
    pthread_attr_destroy(&attr);
}

/*
#include <pthread.h>

void fun(DuplexPipe *dp)
{
    char input[1024];
    int ret;
    while(dp->run)
    {
        memset(input, 0, sizeof(input));
        if((ret = scanf("%s", input)) > 0)
        {
            // if((dp->run))
            // {
            //     if(strstr(input, "exit"))
            //     {
            //         screen_duplex_pclose(dp);
            //         break;
            //     }
            //     else
            //     {
            //         ret = strlen(input);
            //         input[ret] = '\n';
            //         ret = write(dp->fw, input, ret+1);
            //         if(ret <= 0)
            //         {
            //             printf("--------- write err %d / %d ---------\n", 
            //                 ret, waitpid(dp->pid, NULL, WNOHANG|WUNTRACED));
            //         }
            //     }
            // }
            // else
            //     break;

            if((dp->run))
            {
                if(strstr(input, "exit"))
                {
                    screen_duplex_pclose(dp);
                    break;
                }
                else if(input[0] == '1')
                    strcpy(input, "scan\n");
                else if(input[0] == '2')
                    strcpy(input, "scan_result\n");
                else if(input[0] == '0')
                    strcpy(input, "q\n");
                else
                    strcpy(input, "status\n");
                write(dp->fw, input, strlen(input));
            }
            else
                break;
        }
    }

    printf("write exit !\n");
}

int main (void)
{
    int ret = 0, ret2;
    char output[1024];
    pthread_t pfd;

    DuplexPipe dp;
    // if(!screen_duplex_popen(&dp, "python"))
    if(!screen_duplex_popen(&dp, "wpa_cli -i wlan0"))
    {
        printf("popen failure !!\n");
        return 1;
    }

    pthread_create(&pfd, NULL, fun, (void *)&dp);

    while (dp.run)
    {
        do{
            memset(output, 0, sizeof(output));
            if((ret = read(dp.fr, output, sizeof(output))) > 0)
                printf("%s", output);
            else if(ret <= 0)
            {
                printf("--------- read err %d / %d ---------\n", 
                    ret, ret2 = waitpid(dp.pid, NULL, WNOHANG|WUNTRACED));
                if(ret2 < 0 || ret2 == dp.pid)
                {
                    dp.run = false;
                    screen_duplex_pclose(&dp);
                    break;
                }
            }
            printf("\nEND %d\n", ret);
        }while(ret > 0);
        printf("\nEND2 %d\n", ret);
        sleep(1);
    }

    printf("read exit !\n");

    while(1)
    {
        printf("\nmain wait\n");
        sleep(1);
    }
}*/
