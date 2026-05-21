sudo cp 99-touchy-pad.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo usermod -aG plugdev "$USER"   # then log out/in, or use newgrp plugdev
