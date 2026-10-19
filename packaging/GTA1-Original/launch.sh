#!/bin/sh
# GTA-eMiYoolator: the original Windows "Grand Theft Auto.exe" (GTA1) on the
# Miyoo Mini, run by gtashell under box86. Log: gtashell.log (game side) and
# launch.log (box86 and this script) next to this file.

GAMEDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$GAMEDIR" || exit 1

LOG="$GAMEDIR/launch.log"
CPUCLOCK=/mnt/SDCARD/.tmp_update/bin/cpuclock

cleanup() {
	sync
	[ -x "$CPUCLOCK" ] && "$CPUCLOCK" 1200 >/dev/null 2>&1
	echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
}
trap cleanup EXIT INT TERM HUP

# Mini Plus (Wi-Fi) runs at 1800 MHz; the original Mini stays at 1400 MHz
if [ -d /sys/class/net/wlan0 ] || grep -qi plus /proc/cmdline 2>/dev/null; then
	FREQ=1800
else
	FREQ=1400
fi
[ -x "$CPUCLOCK" ] && "$CPUCLOCK" $FREQ >/dev/null 2>&1

unset LD_PRELOAD
export LD_LIBRARY_PATH="/mnt/SDCARD/miyoo/lib:/config/lib:/customer/lib:/mnt/SDCARD/.tmp_update/lib:$LD_LIBRARY_PATH"
export BOX86_LOG=1
export BOX86_NOBANNER=0
export BOX86_LD_LIBRARY_PATH="$GAMEDIR/x86"
chmod +x "$GAMEDIR/box86" "$GAMEDIR/gtashell" 2>/dev/null

{
	echo "=== GTA-eMiYoolator $(cat "$GAMEDIR/VERSION" 2>/dev/null) started $(date) at $FREQ MHz"
	for f in scaling_cur_freq scaling_governor; do
		p=/sys/devices/system/cpu/cpu0/cpufreq/$f
		[ -r "$p" ] && echo "cpu: $f=$(cat "$p")"
	done
	grep -E "MemTotal|MemFree|MemAvailable|SwapTotal|SwapFree" /proc/meminfo
	uname -a
	"$GAMEDIR/box86" "$GAMEDIR/gtashell" "$GAMEDIR/gtashell.ini"
	echo "=== exit code $?"
	grep -E "MemFree|MemAvailable|SwapFree" /proc/meminfo
} > "$LOG" 2>&1
