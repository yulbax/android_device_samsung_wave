#!/sbin/sh

formatting=0

fail() {
	echo "ERROR: $1"
	test $formatting = 1 || echo "Nothing was formatted."
	exit 1
}

fstab_dev() {
	awk -v m="$1" '$1 == m { print $3 ; exit }' /etc/twrp.fstab
}

fs_type() {
	blkid "$1" 2>/dev/null | sed -n 's/.* TYPE="\([^"]*\)".*/\1/p'
}

system=$(fstab_dev /system)
cache=$(fstab_dev /cache)
data=$(fstab_dev /data)

case "$system$cache$data" in
	*wave_sd_*) fail "the SD card layout is unknown, wave_sd.conf was not applied" ;;
esac

boot=${system%p*}p1

for dev in $system $cache $data ; do
	test -b "$dev" || fail "$dev doesn't exist, check wave_sd.conf"
	test "$dev" != "$boot" || fail "$dev is the boot.img partition, check wave_sd.conf"
done
test "$system" != "$cache" -a "$system" != "$data" -a "$cache" != "$data" ||
	fail "system, cache and data must be different partitions, check wave_sd.conf"

for dev in $system $cache $data ; do
	case "$(fs_type $dev)" in
		vfat|msdos|exfat|ntfs)
			fail "$dev holds $(fs_type $dev), it may be your files, check wave_sd.conf" ;;
	esac
done

echo "system $system: ext4 (was $(fs_type $system))"
echo "cache  $cache: ext4 (was $(fs_type $cache))"
echo "data   $data: f2fs (was $(fs_type $data))"

for m in $(awk -v s=$system -v c=$cache -v d=$data \
		'$1 == s || $1 == c || $1 == d { print $2 }' /proc/mounts | sort -r) ; do
	umount $m || fail "can't unmount $m"
done

formatting=1
echo "Formatting system..."
make_ext4fs $system >/dev/null || fail "make_ext4fs failed on $system"
echo "Formatting cache..."
make_ext4fs $cache >/dev/null || fail "make_ext4fs failed on $cache"
echo "Formatting data..."
mkfs.f2fs $data >/dev/null || fail "mkfs.f2fs failed on $data"

mkdir -p /data
if mount -t f2fs $data /data ; then
	mkdir -p /data/media/0
	chown 1023:1023 /data/media /data/media/0
	chmod 0770 /data/media /data/media/0
	restorecon -R /data/media 2>/dev/null
	umount /data
fi
sync

echo "Done. The internal storage is empty now: install the zip"
echo "from the External SD Card or with ADB Sideload."
exit 0
