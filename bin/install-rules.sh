sudo cp 99-touchy-pad.rules /etc/udev/rules.d/
sudo chmod a+r /etc/udev/rules.d/99-touchy-pad.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo usermod -aG plugdev "$USER"   # then log out/in, or use newgrp plugdev
