#!/system/bin/sh
[ "$KSU" = true ] || abort "需要 KernelSU / ReSukiSU"
[ "$(getprop ro.product.device)" = alioth ] || abort "仅支持 Redmi K40 / alioth"
set_perm "$MODPATH/action.sh" 0 0 0755
touch "$MODPATH/skip_mount"
# There are no boot hooks or mounts; managers that support hot install can expose the action now.
export MODULE_HOT_INSTALL_REQUEST=true
ui_print "安装完成：在模块页点击“操作”进入 iOS。"
ui_print "退出 iOS 后自动返回 Android；模块不会在 Android 开机时自动运行。"
