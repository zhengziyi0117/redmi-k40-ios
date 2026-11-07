#!/system/bin/sh
# Only the final bootctl/reboot pair changes boot state. --check is read-only.
set -eu
MODDIR=${0%/*}
export PATH=/data/adb/ksu/bin:/system/bin:/system/xbin:/vendor/bin
fail() { echo "无法进入 iOS：$*" >&2; exit 1; }
case "${1:-}" in
  ''|--boot) mode=boot ;;
  --check) mode=check ;;
  *) fail "参数仅支持 --check 或 --boot" ;;
esac
[ "$(id -u)" = 0 ] || fail "需要 root"
[ "$(getprop ro.product.device)" = alioth ] || fail "机型不是 alioth"
[ "$(getprop ro.boot.slot_suffix)" = _a ] || fail "Android 不是从 A 槽运行"
command -v bootctl >/dev/null || fail "找不到 bootctl"
[ "$(bootctl get-current-slot)" = 0 ] || fail "启动 HAL 未确认 A 槽"
bootctl is-slot-bootable 0 || fail "Android A 槽不可启动"
bootctl is-slot-marked-successful 0 || fail "Android A 槽尚未确认启动成功"
[ "$(bootctl get-snapshot-merge-status)" = none ] || fail "系统更新或快照合并尚未完成"
echo "正在核验 iOS 和 Android 启动镜像…"
[ -s "$MODDIR/expected.sha256" ] || fail "缺少镜像清单，请重新生成模块"
sha256sum -c "$MODDIR/expected.sha256" || fail "镜像已变化，可能发生过系统更新，请重新检查安装"
if [ -f "$MODDIR/boot-prefix.sha256" ]; then
  prefix=$(dd if=/dev/block/by-name/boot_b bs=1048576 count=160 2>/dev/null | sha256sum)
  [ "${prefix%% *}" = "$(cat "$MODDIR/boot-prefix.sha256")" ] || fail "B 槽启动镜像已变化，请重新检查安装"
fi
if [ -f "$MODDIR/cust-prefix.sha256" ]; then
  read -r count expected < "$MODDIR/cust-prefix.sha256"
  [ "$count" = 512 ] || fail "应用存储检查范围无效"
  [ "$(blockdev --getsize64 /dev/block/by-name/cust)" = 1073741824 ] || fail "应用存储分区大小已变化"
  actual=$(dd if=/dev/block/by-name/cust bs=1048576 count=512 2>/dev/null | sha256sum)
  [ "${actual%% *}" = "$expected" ] || fail "iOS 应用镜像或存储标记已变化，请重新检查安装"
fi
echo "正在核验两槽固件…"
for part in abl aop bluetooth cmnlib cmnlib64 devcfg dsp dtbo featenabler hyp imagefv keymaster mdtp mdtpsecapp modem multiimgoem multiimgqti qupfw tz uefisecapp vbmeta vbmeta_system vendor_boot vm-linux xbl xbl_config; do
  a=$(sha256sum "/dev/block/by-name/${part}_a") || fail "读不到 ${part}_a"
  b=$(sha256sum "/dev/block/by-name/${part}_b") || fail "读不到 ${part}_b"
  [ "${a%% *}" = "${b%% *}" ] || fail "${part} 两槽固件不同，拒绝切换"
done
echo "检查通过。退出 iOS 后将返回 Android。"
[ "$mode" = boot ] || exit 0
echo "即将重启进入 iOS…"
sync
if ! bootctl set-active-boot-slot 1; then
  bootctl set-active-boot-slot 0 || echo "恢复 A 槽失败，可用 fastboot set_active a" >&2
  fail "切换 B 槽失败"
fi
if ! reboot; then
  bootctl set-active-boot-slot 0 || echo "恢复 A 槽失败，可用 fastboot set_active a" >&2
  fail "重启失败，已尝试恢复 A 槽"
fi
