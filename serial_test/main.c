#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>

#include <sys/ioctl.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define TIMEOUT                     1  /* read operation timeout 1s = TIMEOUT/10 */
#define MIN_LEN                     128  /* the min len datas */
#define DEV_NAME_LEN                11
#define SERIAL_ATTR_BAUD            115200  
#define SERIAL_ATTR_DATABITS        8
#define SERIAL_ATTR_STOPBITS        1
#define SERIAL_ATTR_PARITY          'n'
#define SERIAL_MODE_NORMAL          0
#define SERIAL_MODE_485             1
#define DELAY_RTS_AFTER_SEND        1   /* 1ms */
#define SER_RS485_ENABLED		    1	/* 485 enable */

typedef struct {
    unsigned int lable;
	unsigned int baudrate;
} SERIAL_BAUD_ST;

typedef struct {
    char parity;
	unsigned int baud;
    unsigned int databits;
    unsigned int stopbits;
} SERIAL_ATTR_ST;

static SERIAL_BAUD_ST g_attr_baud[] = {
    {921600, B921600},
    {460800, B460800},
    {230400, B230400},
    {115200, B115200},
    {57600, B57600},
    {38400, B38400},
    {19200, B19200},
    {9600, B9600},
    {4800, B4800},
    {2400, B2400},
    {1800, B1800},
    {1200, B1200},
};

// static char g_dev_serial[][DEV_NAME_LEN] = {
//         "/dev/ttyS1",
//         "/dev/ttyS2",
//         "/dev/ttyS3",
//         "/dev/ttyS4",
// };

void qbox_delay_us(unsigned int us)
{
    struct timeval delay;
    if (us > 1000000)
    {
        delay.tv_sec = us / 1000000;
        delay.tv_usec = us % 1000000; //us延时
    }
    else
    {
        delay.tv_sec = 0;
        delay.tv_usec = us; //us延时
    }
    select(0, NULL, NULL, NULL, &delay);
}

static int attr_baud_set(int fd, unsigned int baud)
{
    int i; 
    int ret = 0; 
    struct termios option;

    /* get old serial attribute */
    memset(&option, 0, sizeof(option));
    if (0 != tcgetattr(fd, &option)) 
    { 
        printf("tcgetattr failed.\n");
        return -1;  
    }
    
    for (i = 0; i < ARRAY_SIZE(g_attr_baud);  i++) 
	{ 
        if (baud == g_attr_baud[i].lable) 
        {     
            ret = tcflush(fd, TCIOFLUSH);
            if (0 != ret)
            {
                printf("tcflush failed.\n");
                break;
            }
            
            ret = cfsetispeed(&option, g_attr_baud[i].baudrate); 
            if (0 != ret)
            {
                printf("cfsetispeed failed.\n");
                ret = -1;
                break;
            }
            
            ret = cfsetospeed(&option, g_attr_baud[i].baudrate); 
            if (0 != ret)
            {
                printf("cfsetospeed failed.\n");
                ret = -1;
                break;
            }
            
            ret = tcsetattr(fd, TCSANOW, &option);  
            if  (0 != ret) 
            {        
                printf("tcsetattr failed.\n");
                ret = -1;
                break;     
            }    
            
            ret = tcflush(fd, TCIOFLUSH);
            if (0 != ret)
            {
                printf("tcflush failed.\n");
                break;
            }
        }  
    }
    
    return ret;
}

static int attr_other_set(int fd, SERIAL_ATTR_ST *serial_attr)
{ 
	struct termios option;	

    /* get old serial attribute */
    memset(&option, 0, sizeof(option));
    if (0 != tcgetattr(fd, &option)) 
    { 
        printf("tcgetattr failed.\n");
        return -1;  
    }

    option.c_iflag = CLOCAL | CREAD;
    
    /* set datas size */
    option.c_cflag &= ~CSIZE; 
    option.c_iflag = 0;

    switch (serial_attr->databits)
    {   
        case 7:		
            option.c_cflag |= CS7; 
            break;
            
        case 8:     
            option.c_cflag |= CS8;
            break;  
            
        default:    
            printf("invalid argument, unsupport datas size.\n");
            return -1;  
    }

    /* set parity */    
    switch (serial_attr->parity) 
    {   
        case 'n':
        case 'N':    
            option.c_cflag &= ~PARENB;   
            option.c_iflag &= ~INPCK;      
            break;  
            
        case 'o':   
        case 'O':    
            option.c_cflag |= (PARODD | PARENB);  
            option.c_iflag |= INPCK;            
            break;  
            
        case 'e':  
        case 'E':   
            option.c_cflag |= PARENB;       
            option.c_cflag &= ~PARODD;   
            option.c_iflag |= INPCK;     
            break;
            
        case 's': 
        case 'S':  
            option.c_cflag &= ~PARENB;
            option.c_cflag &= ~CSTOPB;
            break;  
            
        default:   
            printf("invalid argument, unsupport parity type.\n");
            return -1;  
    }  
    
    /* set stop bits  */
    switch (serial_attr->stopbits)
    {   
        case 1:    
            option.c_cflag &= ~CSTOPB;  
            break;  
            
        case 2:    
            option.c_cflag |= CSTOPB;  
            break;
            
        default:    
            printf("invalid argument, unsupport stop bits.\n");
            return -1; 
    } 
    
    option.c_oflag = 0;
    option.c_lflag = 0;  
    option.c_cc[VTIME] = TIMEOUT;    
    option.c_cc[VMIN] = MIN_LEN; 

    if (0 != tcflush(fd,TCIFLUSH))   
    { 
        printf("tcflush failed.\n");
        return -1;  
    }
    
    if (0 != tcsetattr(fd, TCSANOW, &option))   
    { 
        printf("tcsetattr failed.\n");
        return -1;  
    }

#if 0	
    tcgetattr(fd, &option);
    printf("c_iflag: %x\rc_oflag: %x\n", option.c_iflag, option.c_oflag);
    printf("c_cflag: %x\nc_lflag: %x\n", option.c_cflag, option.c_lflag);
    printf("c_line: %x\nc_cc[VTIME]: %d\nc_cc[VMIN]: %d\n", option.c_line, option.c_cc[VTIME], option.c_cc[VMIN]);
#endif

    return 0;  
}

static int attr_set(int fd, SERIAL_ATTR_ST *serial_attr)
{
    int ret = 0;
    
	if (NULL == serial_attr)
	{
        printf("invalid argument.\n");
        return -1;  
	}
    
    if (0 == ret)
    {
        ret = attr_baud_set(fd, serial_attr->baud);
        if (0 == ret)
        {
            ret = attr_other_set(fd, serial_attr);
        }
    }

    return ret;
}

#define APP_COMMIT  "使用说明:\r\n./serial_test  串口路径(/dev/ttyS1)  波特率(9600/115200/230400/460800)  禁用输入补全回车(0/1)  转储接收数据到文件(/home/test.txt)  是否HEX格式存储(0/1)\r\n"

static int t_fd = 0, t_fd2 = 0;
static int disUseEnter = 0, hexShow = 0;

///////
void my_handler(int s)
{
    printf("catch \"close\"  ! \r\n");

    if(t_fd > 0)
        close(t_fd);
    if(t_fd2 > 0)
        close(t_fd2);
    
    exit(1);
}
///////

void datas_read(void)
{
    int i, j, tH, tL;
    int ret;
    unsigned char read_buf[10240], read_buf_hex[30720];

    while(1)
    {
        memset(read_buf, 0, sizeof(read_buf));
        ret = read(t_fd, read_buf, sizeof(read_buf));
        if(ret > 0)
        {
            printf("recv %d\r\n", ret);
            printf("%s\r\n", read_buf);
            //
            if(t_fd2 > 0)
            {
                j = 0;
                memset(read_buf_hex, 0, sizeof(read_buf_hex));
                //
                if(hexShow > 0)
                {
                    for(i = 0; i < ret; i++)
                    {
                        tH = read_buf[i]>>4;
                        tL = read_buf[i]&0x0F;
                        read_buf_hex[j++] = tH > 9 ? (tH - 10 + 'A') : tH + '0';
                        read_buf_hex[j++] = tL > 9 ? (tL - 10 + 'A') : tL + '0';
                        read_buf_hex[j++] = ' ';
                    }
                }
                else
                {
                    for(i = 0; i < ret; i++)
                    {
                        if((read_buf[i] < ' ' || read_buf[i] > '~') && 
                            read_buf[i] != '\r' && 
                            read_buf[i] != '\n')
                        {
                            break;
                            // tH = read_buf[i]>>4;
                            // tL = read_buf[i]&0x0F;
                            // read_buf_hex[j++] = '\\';
                            // read_buf_hex[j++] = 'x';
                            // read_buf_hex[j++] = tH > 9 ? (tH - 10 + 'A') : tH + '0';
                            // read_buf_hex[j++] = tL > 9 ? (tL - 10 + 'A') : tL + '0';
                        }
                        else
                            read_buf_hex[j++] = read_buf[i];
                    }
                }
                write(t_fd2, read_buf_hex, j);
            }
        }
    }
}

int rs232_transfer(void)
{
    char buf[1024], spaceBuf[32];
    int ret;
    pthread_t pid;

    /////////
    
    struct sigaction siglntHandler;
    siglntHandler.sa_handler = my_handler;
    sigemptyset(&siglntHandler.sa_mask);
    siglntHandler.sa_flags = 0;
    sigaction(SIGINT, &siglntHandler, NULL);
    
    ////////

    if(pthread_create(&pid, NULL, (void *)&datas_read, NULL) != 0)
    {
		printf("can't create thread datas_read() \r\n");
        return -1;
    }
    
    while(1)
    {
        memset(buf, 0, sizeof(buf));
        ret = 0;
        while(1)
        {
            memset(spaceBuf, 0, sizeof(spaceBuf));
            scanf("%s%[ ]", &buf[ret], spaceBuf);
            ret = strlen(buf);
            if(spaceBuf[0] == 0)
                break;
            strcpy(&buf[ret], spaceBuf);
            ret += strlen(spaceBuf);
        }
        if(!disUseEnter)
        {
            buf[ret]='\r';
            buf[ret+1]='\n';
        }
        //printf("now write : %s\r\n", buf);
        ret = write(t_fd, buf, strlen(buf));
        tcdrain(t_fd);    //等待输出完毕
        sleep(1);
    }
    
    //pthread_join(pid, NULL);
    pthread_cancel(pid);

    return ret;
}

int main(int argc, char *argv[])
{
    int ret = 0, baudrate = 0;

	SERIAL_ATTR_ST serial_attr;

	// 查看说明
	if(argc < 3 || strcmp(argv[1], "-?") == 0)
	{
	    printf("%s", APP_COMMIT);
	    return -1;
	}

	//  打开串口
	t_fd = open(argv[1], O_RDWR);
    if (t_fd <= 0) 
	{
		printf("open serial device %s error !\r\n\r\n", argv[1]);
        printf("%s", APP_COMMIT);
        return -1;
	}
    printf("device : %s\r\n", argv[1]);

    //  波特率
    sscanf(argv[2], "%d", &baudrate);
    if(baudrate != 9600 && 
        baudrate != 115200 && 
        baudrate != 230400 && 
        baudrate != 460800)
    {
        printf("serial baudrate error !\r\n\r\n");
        printf("%s", APP_COMMIT);
        close(t_fd);
        return -1;
    }
    printf("baudrate : %d\r\n", baudrate);

    // 发送时自动补全回车
    if(argc < 4 || argv[3][0] == '0')
    {
        printf("auto insert Enter : No\r\n");
        disUseEnter = 0;
    }
    else
    {
        printf("auto insert Enter : Yes\r\n");
        disUseEnter = 1;
    }

    // 打开log输出文件
    if(argc > 4 && argv[4])
    {
        t_fd2 = open(argv[4], O_RDWR|O_CREAT|O_TRUNC, 777);
        if(t_fd2 <= 0)
        {
            printf("open file %s error !\r\n\r\n", argv[4]);
            printf("%s", APP_COMMIT);
            close(t_fd);
            return -1;
        }
        printf("output file : %s\r\n", argv[4]);
    }
    else
        t_fd2 = 0;

    // HEX显示
    if(argc < 6 || argv[5][0] == '0')
    {
        printf("hex print : No\r\n");
        hexShow = 0;
    }
    else
    {
        printf("hex print : Yes\r\n");
        hexShow = 1;
    }
    
    //
    memset(&serial_attr, 0, sizeof(serial_attr));
    serial_attr.baud = baudrate;
    serial_attr.databits = 8;
    serial_attr.stopbits = 1;
    serial_attr.parity = 'n';
    //
    ret = attr_set(t_fd, &serial_attr);
    if (0 == ret)
        ret = rs232_transfer();
    else
    {
        printf("serial init failed !!\r\n");
        ret = -1;
    }
    
    close(t_fd);
    close(t_fd2);
    
	return ret;
}


