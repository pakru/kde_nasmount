# KDE NAS Mount

Mount SMB network shares from Dolphin in two clicks. No `fstab` editing, and
mounts come back after reboot.

![Network Mounts settings page](docs/img/img2.png)

**Supported:** Kubuntu 26.04+ (`.deb`), Fedora KDE 44+ (`.rpm`). Other distros
can build from source (Plasma 6.0+, Linux 6.8+).

## Install

Replace `<version>` with the number on the
[latest release](https://github.com/pakru/kde_nasmount/releases/latest).

```bash
# Kubuntu 26.04 amd64
wget https://github.com/pakru/kde_nasmount/releases/latest/download/nasmount-amd64-<version>.deb
sudo apt install ./nasmount-amd64-<version>.deb

# Fedora KDE 44 x86_64
wget https://github.com/pakru/kde_nasmount/releases/latest/download/nasmount-fedora44-x86_64-<version>.rpm
sudo dnf install ./nasmount-fedora44-x86_64-<version>.rpm
```

## Usage

1. In Dolphin, browse to an SMB share, right-click a folder and choose
   **Mount as Network Drive…**.

   ![Mount as Network drive in Dolphin](docs/img/img3.png)

2. Pick an empty local folder, enter credentials and access level, click **Mount**.

   ![Mount as Network Drive dialog](docs/img/img1.png)

3. Manage mounts in **System Settings → Network Mounts**, or run `nasmount`.

## How it works

Each share becomes a systemd `.mount`/`.automount` unit pair, with its
credentials in a root-owned file, and is armed at boot. A small polkit-
authenticated helper does all the privileged work.

## Uninstall

Use the package manager. It unmounts and deletes all your shares and their
saved credentials as well:

```bash
sudo apt purge nasmount                    # Kubuntu
sudo dnf remove --no-autoremove nasmount   # Fedora
```

## Feedback

Bug reports, testing results on other distros and pull requests are welcome:
[open an issue](https://github.com/pakru/kde_nasmount/issues). See
[CONTRIBUTING.md](CONTRIBUTING.md) before sending code.

## Source-build requirements

A **Plasma 6+ / KF6+** desktop and **Linux 6.8+** (for `STATX_MNT_ID_UNIQUE`).

<details>
<summary>Build dependencies</summary>

On Kubuntu 26.04, install the packages needed to build, test, and run it:

```bash
sudo apt update
sudo apt install \
  appstream cifs-utils cmake diffutils extra-cmake-modules findutils g++ git make \
  libkf6auth-dev libkf6config-dev libkf6coreaddons-dev libkf6i18n-dev \
  libkf6kcmutils-dev libkf6kio-dev libkf6widgetsaddons-dev polkitd \
  qml6-module-org-kde-kcmutils qml6-module-org-kde-kirigami \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-dialogs \
  qml6-module-qtquick-layouts qt6-base-dev qt6-declarative-dev systemd
```

On Fedora KDE 44, use:

```bash
sudo dnf install \
  appstream cifs-utils cmake diffutils extra-cmake-modules findutils gcc-c++ git make \
  kf6-kauth-devel kf6-kcmutils kf6-kcmutils-devel kf6-kirigami \
  kf6-kconfig-devel kf6-kcoreaddons-devel kf6-ki18n-devel kf6-kio-devel \
  kf6-kwidgetsaddons-devel polkit qt6-qtbase-devel qt6-qtdeclarative \
  qt6-qtdeclarative-devel systemd
```

</details>

## Build and install from source

```bash
make install        # or ./install.sh
```

Run it as your own user, not root: it builds as you and elevates only to install.

## Author and licence

Written by **Pavel Krutikhin** ([@pakru](https://github.com/pakru)),
<krutikhin92@gmail.com>.

Copyright © 2026 Pavel Krutikhin. Licensed under the **GNU General Public
License, version 3 or later** (`GPL-3.0-or-later`); see [LICENSE](LICENSE) for
the full text.
