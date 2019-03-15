#include "wlan.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
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

static ScreenWlan_Struct *wlan = NULL;

//发指令
bool _wpa_cli_cmd(ScreenWlan_Struct *wlan, char *cmd)
{
	if(wlan && wlan->wpa_cli.run && cmd)
	{
		//发指令
		int i = 1;
		while(wlan->cmdReady)//等待空闲
		{
			view_delay_ms(100);
			if(!wlan->scan || ++i > 30)//3秒超时
				return false;
		}
		wlan->cmdReady = 1;//在编辑
		memset(wlan->cmd, 0, WPA_CLI_CMD_LEN);
		strcpy(wlan->cmd, cmd);
		wlan->cmdReady = 2;//发出
		return true;
	}
	return false;
}

//发指令,并等待期望返回 失败返回 NULL 成功返回 wlan->cmdResult 指针
char *_wpa_cli_cmd2(ScreenWlan_Struct *wlan, char *cmd, char *expect)
{
	if(wlan && wlan->wpa_cli.run && expect)
	{
		int i = 0;
		while(wlan->cmdResultReady)//等待空闲
		{
			view_delay_ms(100);
			if(!wlan->scan || ++i > 30)//3秒超时
				return NULL;
		}
		//发出指令
		if(!_wpa_cli_cmd(wlan, cmd))
			return NULL;
		//等待回复
		wlan->cmdResultReady = 1;
		i = 0;
		while(wlan->cmdResultReady == 1)//等待
		{
			view_delay_ms(100);
			if(!wlan->scan || ++i > 50)//5秒超时
			{
				wlan->cmdResultReady = 0;
				return NULL;
			}
		}
		//匹配回复
		if(strstr(wlan->cmdResult, expect))
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

//
int screen_wifi_status(void)
{
	if(!wlan || !wlan->wpa_cli.run)
		return -1;
	
	;

	return 0;
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
	
	char cmd[WPA_CLI_CMD_LEN];

	//关闭扫描
	wlan->scan = false;

	//移除0号网络
	if(!_wpa_cli_cmd(wlan, "remove_network 0\n"))
	{
		printf("remove_network 0 err !\n");
		// return;
	}
	view_delay_ms(1000);
	//添加0号网络
	if(!_wpa_cli_cmd2(wlan, "add_network\n", "OK"))
	{
		printf("add_network err !\n");
		// return;
	}
	view_delay_ms(1000);
	//设置名称
	memset(cmd, 0, WPA_CLI_CMD_LEN);
	snprintf(cmd, WPA_CLI_CMD_LEN, "set_network 0 ssid \"%s\"\n", ssid);
	if(!_wpa_cli_cmd2(wlan, cmd, "OK"))
	{
		printf("set_network 0 ssid \"%s\" err !\n", ssid);
		return;
	}
	view_delay_ms(1000);
	//
	if(key)
	{
		//设置密码
		memset(cmd, 0, WPA_CLI_CMD_LEN);
		snprintf(cmd, WPA_CLI_CMD_LEN, "set_network 0 psk \"%s\"\n", key);
		if(!_wpa_cli_cmd2(wlan, cmd, "OK"))
		{
			printf("set_network 0 psk \"%s\" err !\n", key);
			return;
		}
		//设置密码类型
		memset(cmd, 0, WPA_CLI_CMD_LEN);
		snprintf(cmd, WPA_CLI_CMD_LEN, "set_network 0 key_mgmt WPA-PSK\n");
		if(!_wpa_cli_cmd2(wlan, cmd, "OK"))
		{
			printf("set_network 0 key_mgmt WPA-PSK err !\n");
			return;
		}
	}
	else
	{
		//设置密码类型
		memset(cmd, 0, WPA_CLI_CMD_LEN);
		snprintf(cmd, WPA_CLI_CMD_LEN, "set_network 0 key_mgmt NONE\n");
		if(!_wpa_cli_cmd2(wlan, cmd, "OK"))
		{
			printf("set_network 0 key_mgmt NONE err !\n");
			return;
		}
	}
	view_delay_ms(1000);
	//启动连接
	memset(cmd, 0, WPA_CLI_CMD_LEN);
	snprintf(cmd, WPA_CLI_CMD_LEN, "enable_network 0\n");
	if(!_wpa_cli_cmd2(wlan, cmd, "OK"))
	{
		printf("enable_network 0 err !\n");
		return;
	}
	view_delay_ms(1000);
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

	//移除0号网络
	if(!_wpa_cli_cmd(wlan, "remove_network 0\n"))
	{
		printf("remove_network 0 err !\n");
		return;
	}
}

void _thr_wpa_cli_read(ScreenWlan_Struct *wlan)
{
    char buff[WPA_CLI_CMD_LEN];
    int ret;
    while(wlan->wpa_cli.run)
    {
        memset(buff, 0, WPA_CLI_CMD_LEN);
        ret = read(wlan->wpa_cli.fr, buff, WPA_CLI_CMD_LEN);//此处为阻塞读
        if(ret > 0)
        {
			if(wlan->cmdResultReady == 1)
			{
				memcpy(wlan->cmdResult, buff, WPA_CLI_CMD_LEN);
				wlan->cmdResultReady = 2;
			}
			else if(wlan->scan)
			{
				//整理数据并回调
				;
			}
			//
			printf("%s\n", buff);
        }
        else//检查子进程状态
        {
            if(waitpid(wlan->wpa_cli.pid, NULL, WNOHANG|WUNTRACED) != 0)
                break;
        }
        view_delay_ms(200);
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
			// 	if(strstr(input, "exit"))
			// 	{
			// 		screen_duplex_pclose(dp);
			// 		break;
			// 	}
			// 	else
			// 	{
			// 		ret = strlen(input);
			// 		input[ret] = '\n';
			// 		ret = write(dp->fw, input, ret+1);
			// 		if(ret <= 0)
			// 		{
			// 			printf("--------- write err %d / %d ---------\n", 
			// 				ret, waitpid(dp->pid, NULL, WNOHANG|WUNTRACED));
			// 		}
			// 	}
			// }
			// else
			// 	break;

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
