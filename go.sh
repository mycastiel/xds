set -e

rmmod p2p_dev
rmmod stub

insmod ./p2p_dev.ko
chmod 666 /dev/p2p_device
echo done
