#!/bin/sh
cmd=$(realpath $0)
_DIR=$(dirname $cmd)
cd $_DIR

export PATH=$PATH:/oem/usr/ko/

RTL8812AU_SKIP_RKWIFI_SERVER=0
WIFI_MODE_FILE=/userdata/wifi_mode

insmod_if_present() {
	ko_name="$1"
	shift
	[ -f "/oem/usr/ko/$ko_name" ] || return 0
	insmod "/oem/usr/ko/$ko_name" "$@" 2>/dev/null || true
}

have_cmd() {
	command -v "$1" >/dev/null 2>&1
}

get_wifi_mode() {
	if [ -n "$RK_WIFI_MODE" ]; then
		printf '%s' "$RK_WIFI_MODE"
		return 0
	fi

	if [ -f "$WIFI_MODE_FILE" ]; then
		tr -d ' \t\r\n' < "$WIFI_MODE_FILE"
		return 0
	fi

	return 0
}

should_keep_wlan0_unmanaged() {
	wifi_mode="$(get_wifi_mode)"

	if [ "$RTL8812AU_SKIP_RKWIFI_SERVER" != "1" ]; then
		return 1
	fi

	case "$wifi_mode" in
	managed|sta|client)
		echo "RTL8812AU monitor/WFB mode overridden by '$wifi_mode'. Start managed Wi-Fi stack."
		return 1
		;;
	""|monitor|wfb)
		echo "RTL8812AU detected. Keep wlan0 unmanaged for monitor/WFB use."
		return 0
		;;
	*)
		echo "Unknown Wi-Fi mode '$wifi_mode'. Keep wlan0 unmanaged."
		return 0
		;;
	esac
}

start_wifi_userspace() {
	if ! ip link show wlan0 >/dev/null 2>&1; then
		echo "wlan0 not found. Skip Wi-Fi userspace startup."
		return 0
	fi

	if should_keep_wlan0_unmanaged; then
		return 0
	fi

	if have_cmd rkwifi_server; then
		echo "wlan0 present. Starting rkwifi_server."
		rkwifi_server start &
		return 0
	fi

	if have_cmd wpa_supplicant && [ -f /etc/wpa_supplicant.conf ]; then
		echo "wlan0 present. Starting wpa_supplicant."
		ifconfig wlan0 up
		killall wpa_supplicant 2>/dev/null || true
		killall dhcpcd 2>/dev/null || true
		killall udhcpc 2>/dev/null || true
		rm -rf /var/run/wpa_supplicant 2>/dev/null || true
		mkdir -p /var/run/wpa_supplicant
		wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf >/dev/null 2>&1 || \
			wpa_supplicant -B -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf >/dev/null 2>&1 || \
			return 1
		if have_cmd dhcpcd; then
			dhcpcd wlan0 -AL -t 0 &
		elif have_cmd udhcpc; then
			chmod a+x /usr/share/udhcpc/default.script 2>/dev/null || true
			udhcpc -i wlan0 -T 1 -A 0 -b -q &
		fi
		return 0
	fi

	echo "wlan0 present, but no supported Wi-Fi userspace manager was found."
	return 0
}

load_rtl8812au_module() {
	for module in rtl88xxau_wfb.ko 88XXau_wfb.ko 8812au.ko; do
		if [ -f "/oem/usr/ko/$module" ]; then
			insmod_if_present libarc4.ko
			insmod_if_present cfg80211.ko
			insmod_if_present mac80211.ko
			insmod "/oem/usr/ko/$module"
			case "$module" in
			*_wfb.ko)
				RTL8812AU_SKIP_RKWIFI_SERVER=1
				;;
			esac
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
	# AR9271 with nohwcrypt=1 relies on software CCMP via mac80211/crypto API.
	# Without these crypto modules, ieee80211_key_alloc("ccm(aes)") returns -ENOENT
	# and WPA2 4-way handshake fails at NL80211_CMD_NEW_KEY.
	insmod ctr.ko
	insmod ccm.ko
	insmod libaes.ko
	insmod aes_generic.ko
	insmod mac80211.ko
	insmod ath.ko
	insmod ath9k_hw.ko
	insmod ath9k_common.ko
	# AR9271 fails WPA2 4-way handshake when PTK installation uses hw crypto.
	# Force software crypto so STA mode can connect reliably.
	insmod ath9k_htc.ko nohwcrypt=1
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

start_wifi_userspace
