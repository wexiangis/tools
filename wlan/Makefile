
# CC=
# LIB = ./lib-ubuntu

CC=
LIB = ./lib-pi

# CC=arm-linux-gnueabihf-
# LIB = ./lib-qbox

target:
	@$(CC)gcc -O3 -Wall -o out main.c wlan.c -lpthread -lwpa_client -L$(LIB) -lpthread
