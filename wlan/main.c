
#include <stdio.h>
#include "wlan.h"

int main(void)
{
    char scanfStr[128];
	
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
                    wifi_scan(NULL, NULL, 5);
                else if(scanfStr[1] == '3')
                    //wifi_connect("Guest hi-T", NULL);
                    wifi_connect("OP5", "1234rewq");
                else if(scanfStr[1] == '4')
                    wifi_disconnect();
                else if(scanfStr[1] == '5')
                    wifi_status();
                else if(scanfStr[1] == '0')
                    wifi_exit();
            }
            //
            scanfStr[0] = 0;
        }
    }
	return 0;	
}

