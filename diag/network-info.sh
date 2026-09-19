#!/system/bin/sh
# network-info.sh - read-only WiFi/networking environment probe (NM diag).
#
# Pure busybox (sh + standard applets), no device-side toolchain needed.
# Gathers measured facts for the Wi-Fi bring-up milestone: kernel, module,
# interface + link state, routing/DNS config, and available userspace tools.
# Nothing is modified or created on the device.
#
# Usage (adbd runs as root on the NM diagnostic image):
#   network-info.sh            full probe
#   network-info.sh wifi       wifi-only: module, loader, supplicant status
#   network-info.sh net        link/route/arp + resolv.conf
#   network-info.sh tools      what networking userspace exists

ETCWIFI=/etc/wifi

section() {
  echo
  echo "== $1 =="
}

ifaces() {
  ls /sys/class/net 2>/dev/null | grep -v '^lo$'
}

probe_wifi() {
  section "kernel"
  uname -a 2>&1

  section "wlan module (tiwlan_drv.ko)"
  if ls -l $ETCWIFI/tiwlan_drv.ko 2>/dev/null; then
    strings $ETCWIFI/tiwlan_drv.ko 2>/dev/null | grep vermagic \
      || echo "(vermagic not printed)"
  else
    echo "(tiwlan_drv.ko missing)"
  fi
  grep -i wlan /proc/modules 2>/dev/null || echo "(no wlan module loaded)"

  section "wl1251 stack blobs"
  ls -l $ETCWIFI/tiwlan.ini $ETCWIFI/firmware.bin 2>&1
  sed -n '1,20p' $ETCWIFI/tiwlan.ini 2>/dev/null

  section "wpa_supplicant (TI Station Driver 1271 expected)"
  wpa_supplicant -v 2>&1 || true

  section "wlan_loader"
  P=$(command -v tiwlan_loader)
  if [ "$P" ]; then
    ls -l "$P" 2>&1
    strings "$P" 2>/dev/null | grep -i -m1 -e wlan_loader -e usage \
      || echo "(no usage string present)"
    echo "(not executed: running tiwlan_loader would mutate wlan.driver.status)"
  else
    echo "(tiwlan_loader NOT FOUND)"
  fi

  section "props"
  getprop wifi.interface 2>&1 || true
  getprop wlan.driver.status 2>&1 || true
  getprop dhcp.tiwlan0.ipaddress 2>&1 || true
}

probe_net() {
  section "interfaces"
  ifaces | while read -r IF; do
    echo "--- $IF ---"
    cat /sys/class/net/$IF/operstate 2>/dev/null || echo "operstate: n/a"
    readlink /sys/class/net/$IF/device/driver 2>/dev/null || echo "driver: n/a"
  done

  section "/proc/net/dev"
  cat /proc/net/dev 2>&1

  section "/proc/net/wireless"
  cat /proc/net/wireless 2>&1

  section "/proc/net/route"
  cat /proc/net/route 2>&1

  section "/proc/net/arp"
  cat /proc/net/arp 2>&1

  section "resolv.conf"
  ls -l /etc/resolv.conf 2>&1
  cat /etc/resolv.conf 2>&1
  section "hosts"
  cat /etc/hosts 2>&1
}

probe_tools() {
  section "networking tools"
  for T in ifconfig route ip wpa_supplicant tiwlan_loader iwconfig iwlist \
           iwpriv udhcpc dhcpcd ping wget nslookup traceroute getprop; do
    P=$(command -v "$T")
    echo "$T: ${P:-NOT FOUND}"
  done

  section "tool versions"
  iwconfig --version 2>&1 || iwconfig -h 2>&1 | head -5 || true
  udhcpc --help 2>&1 | head -2 || true
  dhcpcd --version 2>&1 || true

  section "firmware side-channel"
  dmesg 2>/dev/null | grep -i -e wlan -e wl1251 -e wilink | head -20 || \
    echo "(no wlan mentions in dmesg)"
}

case "$1" in
  "wifi") probe_wifi ;;
  "net")  probe_net ;;
  "tools") probe_tools ;;
  "" | "all")
    probe_wifi
    probe_net
    probe_tools
    ;;
  *)
    echo "usage: $0 [all|wifi|net|tools]"
    exit 1
    ;;
esac