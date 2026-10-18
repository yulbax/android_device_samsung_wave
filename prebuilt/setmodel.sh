#!/tmp/busybox sh

# Script that does apply Wave1/Wave2 specific parameters during the boot flash.

# ui_print by Chainfire
OUTFD=$(/tmp/busybox ps | /tmp/busybox grep -v "grep" | /tmp/busybox grep -o -E "/tmp/updater .*" | /tmp/busybox cut -d " " -f 3);
if /tmp/busybox test -e /tmp/update_binary ; then
	OUTFD=$(/tmp/busybox ps | /tmp/busybox grep -v "grep" | /tmp/busybox grep -o -E "update_binary(.*)" | /tmp/busybox cut -d " " -f 3);
fi
ui_print() {
  if [ $OUTFD != "" ]; then
    echo "ui_print ${1} " 1>&$OUTFD;
    echo "ui_print " 1>&$OUTFD;
  else
    echo "${1}";
  fi;
}
ui_print "Applying Wave1/Wave2 specific parameters..."

if /tmp/busybox grep -q wave2 /proc/cpuinfo ; then
	ui_print "Model is GT-S8530"	
	/tmp/busybox ln -f /system/vendor/firmware/nvram_net_s8530.txt /system/vendor/firmware/nvram_net.txt
	/tmp/busybox rm /system/vendor/firmware/bcm4329_s8500.hcd
	/tmp/busybox sed 's/S8500/S8530/g' -i /system/build.prop
	else
	ui_print "Model is GT-S8500"
	/tmp/busybox ln -f /system/vendor/firmware/nvram_net_s8500.txt /system/vendor/firmware/nvram_net.txt
	/tmp/busybox rm /system/vendor/firmware/bcm4329_s8530.hcd
fi

if /tmp/busybox test -e /system/etc/wave-sd-boot ; then
	ui_print "Copying boot.img to the SD card"
	sd_fat=$(/tmp/busybox grep "^/dev/block/mmcblk1p1 " /proc/mounts | /tmp/busybox cut -d " " -f 2)
	sd_fat_mounted=0
	if /tmp/busybox test -z "$sd_fat" ; then
		sd_fat=/tmp/sd_fat
		/tmp/busybox mkdir -p $sd_fat
		/tmp/busybox mount -t vfat /dev/block/mmcblk1p1 $sd_fat || exit 1
		sd_fat_mounted=1
	fi
	/tmp/busybox cp /tmp/boot.img $sd_fat/boot.img || exit 1
	/tmp/busybox sync
	if /tmp/busybox test $sd_fat_mounted = 1 ; then
		/tmp/busybox umount $sd_fat
	fi
else
	ui_print "Flashing boot.img"
	/tmp/flash_image boot /tmp/boot.img
fi


