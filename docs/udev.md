
# Fixing "Insufficient permissions" on Linux

If you see an error like this when you try to talk to your Touchy-Pad on Linux:

```
Insufficient permissions see https://github.com/geeksville/touchy-pad/blob/main/docs/udev.md to fix (/dev/bus/usb/…)
```

it means your normal user account isn't allowed to open the device's USB
interface. This is a one-time fix using a **udev rule** — it tells Linux to
grant access automatically every time the device is plugged in.

> **Other platforms:** This is a Linux-only fix. Windows and macOS don't use
> udev and don't need this step — if you're getting a permission/access error
> there, it's usually a missing USB driver (Windows) or another app already
> holding the device open.

## What the rule does

The Touchy-Pad shows up on the USB bus as `VID:PID 303A:8369`. The kernel
already takes care of the built-in mouse and serial ports, but the custom
command interface is accessed directly by `touchy-pad`, so it needs a rule to
be readable by regular (non-root) users.

The rule is a single file:
[`udev/99-touchy-pad.rules`](https://github.com/geeksville/touchy-pad/blob/main/udev/99-touchy-pad.rules).

## Install it

You'll need a terminal and your `sudo` password. Each step is a single line.

### 1. Download the rule

The easiest way is to grab the raw file straight from GitHub:

```bash
cd /tmp
wget https://raw.githubusercontent.com/geeksville/touchy-pad/main/udev/99-touchy-pad.rules
```

### 2. Copy it where Linux looks for rules

```bash
sudo cp 99-touchy-pad.rules /etc/udev/rules.d/
sudo chmod a+r /etc/udev/rules.d/99-touchy-pad.rules
```

### 3. Reload the rules and apply them now

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
```

### 4. Add your user to the `plugdev` group

The rule grants access to members of the `plugdev` group. Add yourself (you
only need do this once):

```bash
sudo usermod -aG plugdev "$USER"
```

For that change to take effect, **log out and back in** (or reboot). 

### 5. Re-plug the device, then test

Unplug your Touchy-Pad and plug it back in so the new rule applies, then run:

```bash
touchy board-info
```

You should see your board information instead of a permissions error. 🎉

## All-in-one script

If you'd rather not type each command, the repo includes a helper that does
steps 2–4 for you:

```bash
git clone https://github.com/geeksville/touchy-pad.git
cd touchy-pad
sudo bash bin/install-rules.sh
```

(Remember to log out and back in afterward so the group change is picked up.)

## Troubleshooting

- **Still getting the error after re-plugging?** Double-check you logged out
  and back in (or rebooted) after the `usermod` step — group membership isn't
  applied to already-running sessions.
- **Confirm your groups:** run `groups` in a terminal; `plugdev` should appear
  in the list.
- **The `plugdev` group doesn't exist on my system?** Some distributions (e.g.
  Arch) don't create it by default. Create it first.
  ```bash
  sudo groupadd plugdev
  sudo usermod -aG plugdev "$USER"
  ```
- **Nothing works and the device vanishes after a few seconds:** the rule also
  sets `OWNER="1000"`. If your user ID isn't `1000` (run `id -u` to check), the
  ownership line won't match you — rely on the `plugdev` group membership
  instead, which works for any user ID.