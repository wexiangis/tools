#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <termios.h>
#include <linux/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

//#include "./include/common.h"
//#include "./include/at91_gpio.h"

int i2c_transfer_write(int fd, unsigned short slave_addr, unsigned char *data_buf, unsigned int data_len, unsigned char *reg_addr, unsigned int reg_size)
{
    int ret;
    struct i2c_rdwr_ioctl_data write_opt;
    unsigned char *writeBuff;
    unsigned int writeBuffLen;
    //
    writeBuffLen = reg_size + data_len;
    writeBuff = (unsigned char *)malloc((writeBuffLen + 1)*sizeof(unsigned char));
    memcpy(&writeBuff[0], reg_addr, reg_size);
    memcpy(&writeBuff[reg_size], data_buf, data_len);
    //
    memset(&write_opt, 0, sizeof(write_opt));
    write_opt.nmsgs = 1; 
    write_opt.msgs = (struct i2c_msg *)malloc(write_opt.nmsgs * sizeof(struct i2c_msg));
    memset(write_opt.msgs, 0, write_opt.nmsgs * sizeof(struct i2c_msg));
    //
    write_opt.msgs[0].len = writeBuffLen;
    write_opt.msgs[0].flags = !I2C_M_RD;
    write_opt.msgs[0].addr = slave_addr;
    write_opt.msgs[0].buf = writeBuff;
    //
    ret = ioctl(fd, I2C_RDWR, (unsigned long)&write_opt);
    if (write_opt.nmsgs == ret)
        ret = 0;
    else
        ret = -1;
    //
    free(writeBuff);
    if (write_opt.msgs)
    {
        free(write_opt.msgs);
        write_opt.msgs = NULL;
    }
    //
    return ret;
}

int i2c_transfer_read(int fd, unsigned short slave_addr, unsigned char *data_buf, unsigned int data_len, unsigned char *reg_addr, unsigned int reg_size)
{
    int ret;
    struct i2c_rdwr_ioctl_data read_opt;
    //
    memset(&read_opt, 0, sizeof(read_opt));
    read_opt.nmsgs = 2; 
    read_opt.msgs = (struct i2c_msg *)malloc(read_opt.nmsgs * sizeof(struct i2c_msg));
    memset(read_opt.msgs, 0, read_opt.nmsgs * sizeof(struct i2c_msg));
    //
    read_opt.msgs[0].len = reg_size;
    read_opt.msgs[0].flags = 0;
    read_opt.msgs[0].addr = slave_addr;
    read_opt.msgs[0].buf = reg_addr;
    //
    read_opt.msgs[1].len = data_len;
    read_opt.msgs[1].flags = I2C_M_RD;
    read_opt.msgs[1].addr = slave_addr;
    read_opt.msgs[1].buf = data_buf;
    //
    ret = ioctl(fd, I2C_RDWR, (unsigned long)&read_opt);
    if (read_opt.nmsgs == ret)
        ret = 0;
    else
        ret = -1;
    //
    if(read_opt.msgs)
    {
        free(read_opt.msgs);
        read_opt.msgs = NULL;
    }
    //
    return ret;
}

//---------------------------------------------------------------------------------------------------------------------------

int i2c_transfer_open(char *devPath)
{
    return open(devPath, O_RDWR);
}

void i2c_transfer_close(int fd)
{
    close(fd);
}

unsigned int i2c_transfer_stringToUint(char *buf, int hexFlag)
{
    unsigned int ret = 0;
    //
    if(buf == NULL || buf[0] == 0)
        return 0;
    //hex 0xXX
    if(buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X'))
    {
        buf += 2;
        while(*buf)
        {
            if(*buf >= '0' && *buf <= '9')
                ret = ret*16 + *buf - '0';
            else if(*buf >= 'A' && *buf <= 'F')
                ret = ret*16 + 10 + *buf - 'A';
            else if(*buf >= 'a' && *buf <= 'f')
                ret = ret*16 + 10 + *buf - 'a';
            else
                break;
            //
            buf += 1;
        }
    }
    //hex XX
    else if(hexFlag)
    {
        while(*buf)
        {
            if(*buf >= '0' && *buf <= '9')
                ret = ret*16 + *buf - '0';
            else if(*buf >= 'A' && *buf <= 'F')
                ret = ret*16 + 10 + *buf - 'A';
            else if(*buf >= 'a' && *buf <= 'f')
                ret = ret*16 + 10 + *buf - 'a';
            else
                break;
            //
            buf += 1;
        }
    }
    //int
    else
    {
        while(*buf)
        {
            if(*buf >= '0' && *buf <= '9')
                ret = ret*10 + *buf - '0';
            else
                break;
            //
            buf += 1;
        }
    }
    //
    return ret;
}

int i2c_transfer_getModeArray(unsigned char *retBuff, char **buff, int buffLen, int mode)
{
    int ret = 0;
    int count = 0;
    //
    if(retBuff == NULL || buff == NULL || buff[0] == NULL)
        return -1;
    //
    if(mode == 0)
    {
        for(count = 0; buff[0][count] && count < buffLen; count++)
        {
            if(buff[0][count] > ' ' && buff[0][count] <= '~')
                *retBuff++ = buff[0][count];
            else
                break;
        }
        *retBuff = 0;
        return count;
    }
    else if(mode == 1)
    {
        for(count = 0; buff[count] && count < buffLen; count++)
            *retBuff++ = i2c_transfer_stringToUint(buff[count], 0);
        *retBuff = 0;
        return count;
    }
    else if(mode == 2)
    {
        for(count = 0; buff[count] && count < buffLen; count++)
            *retBuff++ = i2c_transfer_stringToUint(buff[count], 1);
        *retBuff = 0;
        return count;
    }
    else
        return -1;
    //
    return 0;
}

char i2c_commit[] = "\r\n---------- 使用说明 ----------\r\n\r\n"
                    "./i2c_transfer /dev/i2c-1(设备) 0x50(设备ID) 0x0000(读/写地址) 2(地址长度1~4) 32(读/写字节数) dataFormat data...\r\n\r\n"
                    "dataFormat: 数据组成方式(读数据时默认用0模式显示)  0: ascii字符串,如:abc  1: 逐个的整形数值,如:113 114 115  2: 逐个的HEX数值,如:71 72 73\r\n"
                    "data: 读数据时不填, 写数据时按 dataFormat 指定的方式传入数据\r\n\r\n"
                    "读数据例子: ./i2c_transfer /dev/i2c-1 0x50 0x0000 2 32 (默认用可见ASCII码方式显示)\r\n"
                    "读数据例子: ./i2c_transfer /dev/i2c-1 0x50 0x0000 2 32 1 (用逐个整形数值的方式显示)\r\n"
                    "读数据例子: ./i2c_transfer /dev/i2c-1 0x50 0x0000 2 32 2 (用逐个HEX数值的方式显示)\r\n\r\n"
                    "写数据例子: ./i2c_transfer /dev/i2c-1 0x50 0x0000 2 3 0 abc (直接输入一串可见的ASCII码)\r\n"
                    "写数据例子: ./i2c_transfer /dev/i2c-1 0x50 0x0000 2 3 1 113 114 115 (指定整形数值的方式输入数据)\r\n"
                    "写数据例子: ./i2c_transfer /dev/i2c-1 0x50 0x0000 2 3 2 71 72 73 (指定HEX数值的方式输入数据)\r\n\r\n"
                    "------------------------------\r\n";

//
int main(int argc, char *argv[])
{
	int fd;
	//
	int i, ret;
	unsigned short id;
	unsigned char addr[4] = {0}, *buf = NULL;
	unsigned int bufLen = 0, addrLen = 0, addr2 = 0;
    int mode = 0;
    int result = -1;
    //
    if(argc < 6)
    {
        printf("%s\r\n", i2c_commit);
        return result;
    }
	//提取参数
	id = i2c_transfer_stringToUint(argv[2], 0);
	addr2 = i2c_transfer_stringToUint(argv[3], 0);
	addrLen = i2c_transfer_stringToUint(argv[4], 0);
	if(addrLen > 4 || addrLen <= 0)
    {
        printf("addr length error !!\r\n");
        return result;
    }
    if(addrLen == 1)
        addr[0] = (unsigned char)(addr2&0xFF);
    else if(addrLen == 2)
    {
        addr[1] = (unsigned char)(addr2&0xFF);
        addr[0] = (unsigned char)((addr2>>8)&0xFF);
    }
    else if(addrLen == 3)
    {
        addr[2] = (unsigned char)(addr2&0xFF);
        addr[1] = (unsigned char)((addr2>>8)&0xFF);
        addr[0] = (unsigned char)((addr2>>16)&0xFF);
    }
    else if(addrLen == 3)
    {
        addr[3] = (unsigned char)(addr2&0xFF);
        addr[2] = (unsigned char)((addr2>>8)&0xFF);
        addr[1] = (unsigned char)((addr2>>16)&0xFF);
        addr[0] = (unsigned char)((addr2>>24)&0xFF);
    }
    //
	bufLen = i2c_transfer_stringToUint(argv[5], 0);
    if(argc > 6)
        mode = i2c_transfer_stringToUint(argv[6], 0);
	buf = (unsigned char *)calloc(bufLen + 64, sizeof(unsigned char));
    if(argc > 7)
    {
        ret = i2c_transfer_getModeArray(buf, &argv[7], bufLen, mode);
        if(ret < 0)
        {
            printf("mode error !!\r\n");
            free(buf);
            return result;
        }
        else if(ret == 0)
        {
            printf("mode analysis error !!\r\n");
            free(buf);
            return result;
        }
        else
            bufLen = ret;
    }
    //
	fd = i2c_transfer_open(argv[1]);    //open(argv[1], O_RDWR);
	if(fd < 0)
	{	
		printf("device %s open error !!\r\n", argv[1]);
		free(buf);
	    return -1;
	}
    //
    if(bufLen > 0)
    {
        //write
        if(argc > 7)
        {
            if(mode == 1)
            {
                for(i = 0; i < bufLen; i++)
                    printf("%d ", buf[i]);
            }
            else if(mode == 2)
            {
                for(i = 0; i < bufLen; i++)
                    printf("%.2X ", buf[i]);
            }
            else
                printf("%s", buf);
            printf("\r\n");
            //
            if(i2c_transfer_write(fd, id, buf, bufLen, addr, addrLen) == 0)
            {
                printf("write success\r\n");
                result = 0;
            }
            else
                printf("write error !!\r\n");
        }
        //read
        else
        {
            memset(buf, 0, bufLen);
            if(i2c_transfer_read(fd, id, buf, bufLen, addr, addrLen) == 0)
            {
                if(mode == 1)
                {
                    for(i = 0; i < bufLen; i++)
                        printf("%d ", buf[i]);
                }
                else if(mode == 2)
                {
                    for(i = 0; i < bufLen; i++)
                        printf("%.2X ", buf[i]);
                }
                else
                    printf("%s", buf);
                //
                printf("\r\nread success\r\n");
                result = 0;
            }
            else
                printf("read error !!\r\n\r\n");
        }
    }
    else
        printf("data Length error !!\r\n");

	//
	free(buf);
	i2c_transfer_close(fd);   //close(fd);
	//
	return result;	
}

