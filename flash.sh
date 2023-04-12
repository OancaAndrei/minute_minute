#!/bin/zsh

./build_copy.sh

disk="invalid"
for d in /dev/disk*; do
    val=$(diskutil info $d | grep -c 'Built In SDXC Reader')
    if [ "$val" -eq "1" ]; then
        disk=$d
        diskutil info $disk
        break
        #echo $disk
    fi
done

if [[ "$disk" == "invalid" ]]; then
    echo "No disk found to flash."
    exit -1
fi

echo "Writing to: $disk"
diskutil unmountDisk $disk
sudo dd if=sdcard.img of="$disk" status=progress
diskutil eject $disk
