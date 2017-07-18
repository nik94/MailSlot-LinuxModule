obj-m += MailDevice.o

all:
	sudo dmesg -c
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules 

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

ins:
	sudo insmod MailDevice.ko

rm:
	sudo rmmod MailDevice.ko
