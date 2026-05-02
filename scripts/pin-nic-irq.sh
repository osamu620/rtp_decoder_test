#!/bin/sh
# Pin a NIC interrupt to a specific CPU.
#
# On ZCU102/Petalinux the eth0 IRQ defaults to CPU 0, where general system
# work also runs. Under high-bitrate ingress (800+ Mbps) the softirq draining
# the NIC RX ring runs late and packets are lost in the ring — invisible at
# the UDP layer (/proc/net/udp drops=0) but visible as net=N in rtp_decoder.
# Moving the IRQ to an otherwise-idle CPU clears it.
#
# Usage:   sudo ./scripts/pin-nic-irq.sh [iface] [cpu]
# Default: iface=eth0, cpu=1
#
# Not persistent across reboot. Re-run after each boot, or wire into a systemd
# unit / /etc/network/if-up.d/ script.

set -eu

IFACE="${1:-eth0}"
CPU="${2:-1}"

if ! [ "$CPU" -ge 0 ] 2>/dev/null; then
  echo "error: cpu must be a non-negative integer, got '$CPU'" >&2
  exit 2
fi

# /proc/interrupts lines look like:
#   51:  32452857  0  0  0  GICv2  95 Level  eth0, eth0
# grep -w treats commas/whitespace as word boundaries, so this matches both
# "eth0" and "eth0," but not "eth01".
IRQ=$(grep -w "$IFACE" /proc/interrupts | awk -F: '{gsub(/ /,"",$1); print $1; exit}')

if [ -z "${IRQ:-}" ]; then
  echo "error: no IRQ found for interface '$IFACE' in /proc/interrupts" >&2
  echo "available interfaces:" >&2
  awk '/[a-z]+[0-9]+(,|$)/ {for (i=NF;i>0;i--) if ($i ~ /^[a-z]+[0-9]+/) {print "  " $i; break}}' \
    /proc/interrupts | sort -u >&2
  exit 1
fi

AFFINITY="/proc/irq/$IRQ/smp_affinity"
MASK=$(printf '%x' $((1 << CPU)))

if [ ! -w "$AFFINITY" ]; then
  echo "error: cannot write $AFFINITY (need root: re-run with sudo)" >&2
  exit 1
fi

PREV=$(cat "$AFFINITY")
echo "$MASK" > "$AFFINITY"
NEW=$(cat "$AFFINITY")

echo "IRQ $IRQ ($IFACE): smp_affinity ${PREV} -> ${NEW} (CPU ${CPU})"
