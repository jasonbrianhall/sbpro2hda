qemu-system-i386 \
  -machine pc,accel=tcg \
  -cpu pentium3 \
  -m 64 \
  -rtc base=localtime,clock=vm,driftfix=none \
  -audiodev id=hostaudio,driver=pa \
  -device intel-hda,id=hda0 \
  -device hda-duplex,audiodev=hostaudio,bus=hda0.0 \
  -hda /var/lib/libvirt/images/freedos14.qcow2 \
  -boot c

I'm using this to test since it prevents double keypresses which is a problem on raw QEMU; also tested under VirtualBox
