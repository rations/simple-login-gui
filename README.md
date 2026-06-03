# simple-login-gui

A minimal GTK graphical login manager for Devuan Excalibur using sysvinit and seatd. Replaces the text console login on tty1 with a simple username/password screen. On logout from the window manager, the login screen reappears automatically. Supports GTK2 and GTK3.

No elogind. No polkit. No ConsoleKit2.

---

## Requirements

- Devuan Excalibur (sysvinit)
- [XLibre](https://x11libre.net/) (recommended) or Xorg — must have libseat support
- seatd
- GTK 2 or GTK 3

## How it works

```
inittab (tty1)
    └── xlogin-launcher
            ├── starts seatd (if not running)
            ├── starts XLibre/Xorg on :0 with seatd seat management
            └── execs xlogin (GTK login window)
                    └── on successful login: forks, drops privileges,
                        execs ~/.xinitrc as the user on display :0
                        └── on logout: login window reappears
```

PAM handles authentication. The session runs entirely as the logged-in user with a clean, minimal environment. The X server stays running between logins.

---

## Installation

Download the release tarball, extract it, and run the installer:

```sh
tar -xf simple-login-gui-1.0.1.tar.gz
cd simple-login-gui-1.0.1
sudo bash install.sh
```

The installer will ask:

1. Which X server you are using (XLibre or Xorg)
2. Which GTK version to use (GTK3 recommended; GTK2 for minimal systems)
3. Which username to configure for graphical login
4. Which session to use (reads `/usr/share/xsessions/` — any installed WM or DE appears here)
5. Whether to start the session inside a D-Bus session bus (recommended — enables trash, removable media, and other GVfs-backed features in file managers such as pcmanfm)

It will then:

- Detect your GPU driver and write `/etc/xlogin.conf` with appropriate X server flags
- Install runtime dependencies
- Build from source, or use a prebuilt binary if one matching your GTK version is present
- Install binaries to `/usr/local/bin/`
- Install PAM config to `/etc/pam.d/xlogin`
- Install the sysvinit service file to `/etc/init.d/xlogin-launcher`
- Enable seatd to start at boot
- Add the configured user to the `input` and `video` groups
- Write `~/.xinitrc` for the target user and `/etc/skel/.xinitrc` for future users
- Update `/etc/inittab` to replace the tty1 getty with xlogin-launcher

Reboot to activate the graphical login screen.

### Session detection

The installer reads `/usr/share/xsessions/*.desktop` to find available sessions. Any properly packaged window manager or desktop environment installs a file there, so the installer works with whatever is on the system — JWM, Openbox, XFCE4, MATE, LXDE, LXQt, i3, and anything else.

Install your preferred WM or DE before running the installer. If no sessions are found, the installer will ask you to enter a session command manually.

### D-Bus session

When prompted, choosing yes wraps the session in `dbus-run-session`, which starts a D-Bus session bus and tears it down cleanly on logout. This is needed for trash, removable media handling, and other GVfs-backed features in file managers such as pcmanfm. Choosing no launches the session directly with no D-Bus bus — suitable for minimal setups that do not need these features.

### Prebuilt binaries

If you build both GTK versions ahead of time, the installer will use the matching prebuilt binary without requiring build tools on the target machine:

```sh
make both        # builds xlogin-gtk3 and xlogin-gtk2
```

### XLibre

XLibre is not in the standard Devuan apt repositories. Install it from the XLibre Devuan repository before rebooting:

```sh
# See https://x11libre.net/ for current repository instructions
```

The launcher detects XLibre automatically and prefers it over Xorg.

---

## Uninstall

Run as root from the project directory:

```sh
sudo bash uninstall.sh
```

This will:

- Remove `/usr/local/bin/xlogin` and `/usr/local/bin/xlogin-launcher`
- Remove `/etc/pam.d/xlogin` and `/etc/xlogin.conf`
- Remove and disable `/etc/init.d/xlogin-launcher`
- Restore `/etc/inittab` (removes the xlogin-launcher line, uncomments the tty1 getty)
- Reload inittab with `telinit q`

User `~/.xinitrc` files and `/etc/skel/.xinitrc` are not removed. Users remain in the `input` and `video` groups. To remove a user from those groups:

```sh
gpasswd -d <username> input
gpasswd -d <username> video
```

Reboot to return to the text console login.

---

## Manual installation

```sh
# Install build dependencies (GTK3)
apt-get install -y libgtk-3-dev libpam0g-dev build-essential gcc make

# Or for GTK2
apt-get install -y libgtk2.0-dev libpam0g-dev build-essential gcc make

# Build (defaults to GTK3)
make

# Or explicitly choose a version
make GTK_VERSION=3   # produces xlogin-gtk3
make GTK_VERSION=2   # produces xlogin-gtk2

# Install (installs the chosen version as /usr/local/bin/xlogin)
sudo make install
sudo make install GTK_VERSION=2

# Install the init.d service file and enable seatd
sudo install -m 755 etc_init.d_xlogin-launcher /etc/init.d/xlogin-launcher
sudo LC_ALL=C update-rc.d seatd defaults
```

Then edit `/etc/inittab` manually: comment out the tty1 getty line and add:

```
1:2345:respawn:/usr/local/bin/xlogin-launcher
```

Write `/etc/xlogin.conf` (use the second form for nvidia):

```sh
# Open-source GPU (AMD, Intel, modesetting)
echo 'XSERVER_FLAGS="-seat seat0 -keeptty -nolisten tcp -ac"' > /etc/xlogin.conf

# nvidia proprietary driver
echo 'XSERVER_FLAGS="-nolisten tcp -ac"' > /etc/xlogin.conf
```

---

## Session configuration

The login manager looks for a session script in this order:

1. `~/.xinitrc` — user's own session script (preferred)
2. `/etc/X11/xinit/xinitrc` — system default
3. Common window managers in order: `jwm`, `openbox-session`, `startxfce4`, `mate-session`
4. `xterm` — last resort

A minimal `~/.xinitrc`:

```sh
#!/bin/sh
exec openbox-session
```

Make it executable:

```sh
chmod 755 ~/.xinitrc
```

To add another user after installation:

```sh
sudo usermod -aG input,video <username>
sudo cp /etc/skel/.xinitrc /home/<username>/.xinitrc
sudo chown <username>:<username> /home/<username>/.xinitrc
```

---

## Security notes

- Authentication is handled entirely by PAM (`/etc/pam.d/xlogin`)
- The xlogin binary runs as root (started by inittab), not setuid — PAM requires root to read `/etc/shadow`, and X must be started before a user is known. This is the same model as traditional display managers (xdm, slim, ldm). The attack surface is physical-only: X is started with `-nolisten tcp` and the machine must be locally accessible
- Privilege drop follows the correct order: `setgid` → `initgroups` → `setuid`
- The child process environment is fully cleared before privilege drop
- All inherited file descriptors are closed before exec
- X access control is disabled (`-ac`) — safe for a single-seat local machine
- The binary is built with stack protection, FORTIFY_SOURCE, PIE, and full RELRO

### nvidia proprietary driver

The nvidia proprietary DDX driver does not support libseat device management. The installer
detects this automatically and writes `/etc/xlogin.conf` with seatd integration disabled
(`-seat seat0 -keeptty` omitted). seatd continues to run and is available for Wayland
compositors started from the user session. Open-source GPU drivers (AMD, Intel, modesetting)
use full seatd integration.

---

## Troubleshooting

**Login screen does not appear after reboot**
- Check that XLibre or Xorg is installed: `command -v Xlibre || command -v Xorg`
- Check seatd is running: `pgrep seatd`
- Check `/var/log/syslog` for xlogin-launcher errors
- Test the launcher manually from a tty as root: `/usr/local/bin/xlogin-launcher`

**Authentication always fails**
- Verify `/etc/pam.d/xlogin` is installed
- Test PAM directly: `pamtester xlogin <username> authenticate`
- Ensure the user's password is set: `passwd <username>`

**Window manager does not start after login**
- Check `~/.xinitrc` exists and is executable (`chmod 755 ~/.xinitrc`)
- Test it manually: `DISPLAY=:0 sh ~/.xinitrc`

**Keyboard or mouse not working in the session**
- Ensure the user is in the `input` group: `groups <username>`
- Add if missing: `sudo usermod -aG input <username>` then log out and back in

---

## License

GPL-2.0 — see [LICENSE](LICENSE)
