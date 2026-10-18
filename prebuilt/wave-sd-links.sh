#!/tmp/busybox sh

OUTFD=$(/tmp/busybox ps | /tmp/busybox grep -v "grep" | /tmp/busybox grep -o -E "/tmp/updater .*" | /tmp/busybox cut -d " " -f 3);
if /tmp/busybox test -e /tmp/update_binary ; then
	OUTFD=$(/tmp/busybox ps | /tmp/busybox grep -v "grep" | /tmp/busybox grep -o -E "update_binary(.*)" | /tmp/busybox cut -d " " -f 3);
fi
ui_print() {
  if [ "$OUTFD" != "" ]; then
    echo "ui_print ${1} " 1>&$OUTFD;
    echo "ui_print " 1>&$OUTFD;
  else
    echo "${1}";
  fi;
}

fail() {
	ui_print "$1"
	exit 1
}

fstab_dev() {
	/tmp/busybox awk -v m="$1" '
		$2 == m && $1 ~ /^\/dev\// { print $1 ; exit }
		$1 == m && $3 ~ /^\/dev\// { print $3 ; exit }' /etc/recovery.fstab
}

foreign_fs() {
	if /tmp/busybox dd if=$1 bs=1 skip=1080 count=2 2>/dev/null | /tmp/busybox od -A n -t x1 | /tmp/busybox grep -q "53 ef" ; then
		return
	fi
	case "$(/tmp/busybox dd if=$1 bs=1 skip=3 count=8 2>/dev/null)" in
		EXFAT*) echo exFAT ; return ;;
		NTFS*) echo NTFS ; return ;;
	esac
	if /tmp/busybox dd if=$1 bs=1 skip=82 count=5 2>/dev/null | /tmp/busybox grep -q FAT32 ||
		/tmp/busybox dd if=$1 bs=1 skip=54 count=3 2>/dev/null | /tmp/busybox grep -q FAT ; then
		echo FAT
	fi
}

system=$(fstab_dev /system)
cache=$(fstab_dev /cache)
data=$(fstab_dev /data)

case "$system" in
	""|/dev/block/wave_sd_*)
		fail "This recovery doesn't know the SD card layout, install from the recovery of this build" ;;
esac

boot=${system%p*}p1

for dev in $system $cache $data ; do
	/tmp/busybox test -b $dev || fail "$dev doesn't exist, check wave_sd.conf"
	/tmp/busybox test $dev != $boot || fail "$dev is the boot.img partition, check wave_sd.conf"
done
/tmp/busybox test $system != $cache -a $system != $data -a $cache != $data ||
	fail "system, cache and data must be different partitions, check wave_sd.conf"

for dev in $system $cache ; do
	fs=$(foreign_fs $dev)
	/tmp/busybox test -z "$fs" || fail "$dev holds $fs, refusing to format it, check wave_sd.conf"
done

ui_print "SD card: system $system, cache $cache, data $data"
/tmp/busybox ln -sf $system /dev/block/wave_sd_system
/tmp/busybox ln -sf $cache /dev/block/wave_sd_cache
/tmp/busybox ln -sf $data /dev/block/wave_sd_data
/tmp/busybox ln -sf $boot /dev/block/wave_sd_boot
