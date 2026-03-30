#!/bin/sh
cmd=$(realpath $0)
_DIR=$(dirname $cmd)
cd $_DIR

export PATH=$PATH:/oem/usr/ko/

RTL8812AU_SKIP_RKWIFI_SERVER=0

insmod_if_present() {
	ko_name="$1"
	shift
	[ -f "/oem/usr/ko/$ko_name" ] || return 0
	insmod "/oem/usr/ko/$ko_name" "$@" 2>/dev/null || true
}

load_rtl8812au_module() {
	for module in rtl88xxau_wfb.ko 88XXau_wfb.ko 8812au.ko; do
		if [ -f "/oem/usr/ko/$module" ]; then
			insmod_if_present libarc4.ko
			insmod_if_present cfg80211.ko
			insmod_if_present mac80211.ko
			insmod "/oem/usr/ko/$module"
			RTL8812AU_SKIP_RKWIFI_SERVER=1
			return 0
		fi
	done

	return 1
}

#for fastboot
#insmod_wifi.ko ${RK_ENABLE_WIFI_CHIP} ${RK_ENABLE_FASTBOOT}
if [ "${1}"x = "y"x ]; then
	case "$2" in
	ATBM6441)
		insmod dw_mmc.ko
		insmod dw_mmc-pltfm.ko
		insmod dw_mmc-rockchip.ko
		insmod cfg80211.ko
		insmod atbm6041_wifi_sdio.ko
		rkwifi_server start &
		exit 0
		;;
	HI3861L)
		insmod dw_mmc.ko
		insmod dw_mmc-pltfm.ko
		insmod dw_mmc-rockchip.ko
		# cat /sys/bus/sdio/devices/*/uevent | grep "0296:5347"
		insmod /oem/usr/ko/hichannel.ko hi_rk_irq_gpio=40
		rkwifi_server start &
		exit 0
		;;
	"")
		;;
	*)
		echo "No dedicated fastboot Wi-Fi init for chip $2, continue autodetect."
		;;
	esac
fi

#AIC8800DW
cat /sys/bus/sdio/devices/*/uevent | grep "8800"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod aic_load_fw.ko aic_fw_path=/oem/usr/ko/
	insmod aic8800_fdrv.ko
	insmod bcmdhd.ko
fi

#AP6XXX
cat /sys/bus/sdio/devices/*/uevent | grep -i "02d0"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod bcmdhd.ko
fi

#rtl8723bs
cat /sys/bus/sdio/devices/*/uevent | grep "024C:B723"
if [ $? -eq 0 ]; then
	insmod libarc4.ko
	insmod cfg80211.ko
	insmod mac80211.ko
	insmod r8723bs.ko
fi

#rtl8723ds
cat /sys/bus/sdio/devices/*/uevent | grep "024C:D723"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod 8723ds.ko
fi

#rtl8189fs
cat /sys/bus/sdio/devices/*/uevent | grep "024C:F179"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod 8189fs.ko
fi

#rtl18188fu
cat /sys/bus/usb/devices/*/uevent | grep "bda\/f179"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod 8188fu.ko
fi

#rtl8812au
cat /sys/bus/usb/devices/*/uevent | grep -Ei "PRODUCT=(0?bda/(8812|881a|a811|b812)|2357/010d|2001/3313|7392/a812|04ca/2006|0409/0408)"
if [ $? -eq 0 ]; then
	load_rtl8812au_module
fi

#ssv6115
cat /sys/bus/usb/devices/*/uevent | grep "8065\/6011"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod ssv6115.ko
fi

#ssv6x5x
cat /sys/bus/usb/devices/*/uevent | grep "8065\/6000"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod libarc4.ko
	insmod mac80211.ko
	insmod ctr.ko
	insmod ccm.ko
	insmod libaes.ko
	insmod aes_generic.ko
	insmod ssv6x5x.ko
fi

#ath9k_htc ar9271
cat /sys/bus/usb/devices/*/uevent | grep -i "cf3\/9271"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod libarc4.ko
	insmod mac80211.ko
	insmod ath.ko
	insmod ath9k_hw.ko
	insmod ath9k_common.ko
	insmod ath9k_htc.ko
fi

#atbm603x
cat /sys/bus/sdio/devices/*/uevent | grep "007A\:6011"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod libarc4.ko
	insmod ctr.ko
	insmod ccm.ko
	insmod libaes.ko
	insmod aes_generic.ko
	insmod atbm603x_.ko
fi

#aic8800
if [ -n "$(cat /proc/device-tree/model | grep "W")" ] || \
[ -n "$(cat /sys/bus/sdio/devices/*/uevent | grep "C8A1\:C18D")" ]; then
	insmod cfg80211.ko
	insmod libarc4.ko
	insmod ctr.ko
	insmod ccm.ko
	insmod libaes.ko
	insmod aes_generic.ko
	insmod aic8800_bsp.ko
	sleep 0.2
	insmod aic8800_fdrv.ko
	sleep 2
	insmod aic8800_btlpm.ko
	sleep 0.1
fi

# WFB-only image: keep driver autoload, but never start the managed Wi-Fi stack.
if ifconfig wlan0 2>&1 | grep -q "not found"; then
	echo "wlan0 not found. Skip Wi-Fi userspace startup."
elif [ "$RTL8812AU_SKIP_RKWIFI_SERVER" = "1" ]; then
	echo "RTL8812AU detected. Keep wlan0 unmanaged for monitor/WFB use."
else
	echo "wlan0 present. Skip rkwifi_server and leave interface unmanaged."
fi
