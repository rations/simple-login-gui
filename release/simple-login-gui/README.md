# simple-login-gui

Minimal GTK3 login manager for Devuan Excalibur + xlibre + seatd. No elogind, no systemd.

---

## Prerequisites
Devuan Excalibur base install (no desktop, boots to tty)

### 1. Enable contrib/non-free
```bash
sudo nano /etc/apt/sources.list
```
```
deb http://deb.devuan.org/merged excalibur main contrib non-free non-free-firmware
deb http://deb.devuan.org/merged excalibur-updates main contrib non-free non-free-firmware
deb http://deb.devuan.org/merged excalibur-backports main contrib non-free non-free-firmware
```

### 2. Install xlibre
```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl

sudo install -m 0755 -d /usr/share/keyrings
curl -fsSL https://mrchicken.nexussfan.cz/publickey.asc | gpg --dearmor | sudo tee /usr/share/keyrings/NexusSfan.pgp > /dev/null
sudo chmod a+r /usr/share/keyrings/NexusSfan.pgp

sudo tee /etc/apt/sources.list.d/xlibre-debian.sources << EOF
Types: deb
URIs: https://xlibre-debian.github.io/devuan/
Suites: main
Components: stable
Signed-By: /usr/share/keyrings/NexusSfan.pgp
EOF

sudo apt-get update
sudo apt-get install xlibre xlibre-archive-keyring

# For AMD GPUs:
sudo apt-get -t excalibur-backports install firmware-amd-graphics
```

### 3. Install seatd
```bash
sudo apt-get install seatd libseat1
sudo adduser root video
```

---

## Installation

### Install from Release (Recommended)

```bash
tar -xf simple-login-gui.tar.gz
sudo ./install.sh
```

### Automated Install (from source)
```bash
git clone https://github.com/rations/simple-login-gui.git
sudo ./install.sh
```

### Manual Install (from source)
```bash
# Dependencies
sudo apt-get install libgtk-3-dev libpam0g-dev build-essential

# Build
make

# Install
sudo make install
```

### Enable login manager
```bash
sudo nano /etc/inittab
```
Comment out getty on tty1 and add xlogin-launcher:
```
#1:2345:respawn:/sbin/getty 38400 tty1
1:2345:respawn:/usr/local/bin/xlogin-launcher
```


---

## Post Install
Install your window manager:
```bash
sudo apt-get install jwm
# OR
sudo apt-get install openbox
```

Add window manager to `~/.xinitrc` for your user:
```bash
echo "exec jwm" > ~/.xinitrc
chmod +x ~/.xinitrc
```

---

## Logout behaviour
When user logs out from window manager, you will be automatically returned to the login screen.

---


## Security
- Correct privilege dropping order: `setgid()` → `initgroups()` → `setuid()`
- Full environment sanitization executed *before* privilege changes
- PAM session kept open for full user session lifetime
- All inherited file descriptors closed before user execution
- Password memory securely wiped after authentication
- No setuid GTK execution path
- Runtime directory created with correct 0700 permissions
- Non-interactive PAM flags to prevent hangs

---

## Uninstall
```bash
sudo make uninstall
# Restore /etc/inittab
```
