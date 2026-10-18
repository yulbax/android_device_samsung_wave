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
	if /tmp/busybox dd if=$1 bs=1 skip=1080 count=2 2>/dev/null | /tmp/busybox od -b | /tmp/busybox grep -q " 123 357" ; then
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

is_f2fs() {
	/tmp/busybox dd if=$1 bs=1 skip=1024 count=4 2>/dev/null | /tmp/busybox od -b | /tmp/busybox grep -q " 020 040 365 362"
}

if [ "$1" = data ]; then
	data=$(/tmp/busybox readlink /dev/block/wave_sd_data)
	cache=$(/tmp/busybox readlink /dev/block/wave_sd_cache)
	if is_f2fs $data ; then
		ui_print "Data is f2fs already, keeping it"
		exit 0
	fi
	fs=$(foreign_fs $data)
	/tmp/busybox test -z "$fs" || fail "$data holds $fs, refusing to format it, check wave_sd.conf"
	zip=$(/tmp/busybox ps | /tmp/busybox grep -v grep | /tmp/busybox grep -o -E "/tmp/(updater|update_binary) .*" | /tmp/busybox cut -d " " -f 4)
	zip=$(/tmp/busybox readlink -f "$zip")
	/tmp/busybox test -n "$zip" || fail "Can't tell where this zip is, nothing was changed"
	for m in $(/tmp/busybox awk -v d=$data '$1 == d { print $2 }' /proc/mounts) ; do
		case "$zip" in
			$m/*) fail "Data must become f2fs, but this zip is on it. Copy the zip to the External SD storage and install it from there" ;;
		esac
	done
	ui_print "Formatting data as f2fs (it was not), and cache"
	for m in $(/tmp/busybox awk -v d=$data -v c=$cache '$1 == d || $1 == c { print $2 }' /proc/mounts | /tmp/busybox sort -r) ; do
		/tmp/busybox umount $m
	done
	/tmp/busybox grep -q "^$data " /proc/mounts && fail "Can't unmount data, nothing was changed"
	/tmp/mkfs.f2fs $data >/dev/null || fail "mkfs.f2fs failed on $data"
	/tmp/busybox grep -q "^$cache " /proc/mounts || /tmp/make_ext4fs $cache >/dev/null ||
		ui_print "Couldn't format cache, wipe it from the recovery"
	exit 0
fi

if [ "$1" = boot ]; then
	dir=/tmp/wave_sd_boot
	/tmp/busybox mkdir -p $dir
	/tmp/busybox mount -t vfat /dev/block/wave_sd_boot $dir ||
		fail "Can't mount the boot.img partition, copy boot.img to it yourself"
	if ! /tmp/busybox cp /tmp/boot.img $dir/boot-new.img ; then
		/tmp/busybox rm -f $dir/boot-new.img
		/tmp/busybox umount $dir
		fail "No room for boot.img on the boot.img partition"
	fi
	if /tmp/busybox test -e $dir/boot.img ; then
		/tmp/busybox rm -f $dir/boot-prev.img
		/tmp/busybox mv $dir/boot.img $dir/boot-prev.img
	fi
	/tmp/busybox mv $dir/boot-new.img $dir/boot.img
	/tmp/busybox sync
	/tmp/busybox umount $dir
	ui_print "boot.img written to the SD card, the old one is boot-prev.img"
	exit 0
fi

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
