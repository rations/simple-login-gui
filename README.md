# simple-login-gui

A minimal GTK3 graphical login manager for Devuan Excalibur using sysvinit and seatd. Replaces the text console login on tty1 with a simple username/password screen. On logout from the window manager, the login screen reappears automatically.

No elogind. No polkit. No ConsoleKit2.

---

## Requirements

- Devuan Excalibur (sysvinit)
- [XLibre](https://x11libre.net/) (recommended) or Xorg — must have libseat support
- seatd
- GTK 3

## How it works

```
inittab (tty1)
    └── xlogin-launcher
            ├── starts seatd (if not running)
            ├── starts XLibre/Xorg on :0 with seatd seat management
            └── execs xlogin (GTK3 login window)
                    └── on successful login: forks, drops privileges,
                        execs ~/.xinitrc as the user on display :0
                        └── on logout: login window reappears
```

PAM handles authentication. The session runs entirely as the logged-in user with a clean, minimal environment. The X server stays running between logins.

---

## Installation

Run as root from the project directory:

```sh
sudo bash install.sh
```

The installer will:

1. Install runtime dependencies (`libgtk-3-0`, `libpam0g`, `seatd`, `libseat1`, `xinit`, `xterm`, `x11-xserver-utils`)
2. Build from source (or use prebuilt binaries if present)
3. Install binaries to `/usr/local/bin/`
4. Install PAM config to `/etc/pam.d/xlogin`
5. Install the sysvinit service file to `/etc/init.d/xlogin-launcher`
6. Enable seatd to start at boot
7. Ask for the username to configure
8. Add that user to the `input` and `video` groups
9. Detect installed window managers and ask which one to use — or offer to install one if none are found
10. Write `~/.xinitrc` for the target user and `/etc/skel/.xinitrc` for future users
11. Update `/etc/inittab` to replace the tty1 getty with xlogin-launcher

Reboot (or log out) to activate the graphical login screen.

### Supported window managers

The installer detects and configures any of:

| Window manager | Package   | Session command     |
|----------------|-----------|---------------------|
| JWM            | `jwm`     | `jwm`               |
| Openbox        | `openbox` | `openbox-session`   |
| XFCE4          | `xfce4`   | `startxfce4`        |
| MATE           | `mate-desktop-environment` | `mate-session` |

If none are installed, the installer offers to install one via apt.

### XLibre

XLibre is not in the standard Devuan apt repositories. Install it from the XLibre Devuan repository before rebooting:

```sh
# See https://x11libre.net/ for current repository instructions
```

The launcher detects XLibre automatically and prefers it over Xorg.

---

## Manual installation

```sh
# Install build dependencies
apt-get install -y libgtk-3-dev libpam0g-dev build-essential gcc make

# Build
make

# Install binaries and PAM config
sudo make install

# Install the init.d service file
sudo install -m 755 etc_init.d_xlogin-launcher /etc/init.d/xlogin-launcher
sudo update-rc.d seatd defaults
```

Then edit `/etc/inittab` manually: comment out the tty1 getty line and add:

```
1:2345:respawn:/usr/local/bin/xlogin-launcher
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

## Uninstall

```sh
sudo make uninstall
sudo rm -f /etc/init.d/xlogin-launcher
sudo update-rc.d xlogin-launcher remove
```

Restore `/etc/inittab` manually: uncomment the tty1 getty line and remove the xlogin-launcher line, then run `sudo telinit q`.

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
