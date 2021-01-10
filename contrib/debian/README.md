
Debian
====================
This directory contains files used to package yieldsakingwalletd/yieldstakingwallet-qt
for Debian-based Linux systems. If you compile yieldsakingwalletd/yieldstakingwallet-qt yourself, there are some useful files here.

## yieldstakingwallet: URI support ##


yieldstakingwallet-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install yieldstakingwallet-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your yieldstakingwallet-qt binary to `/usr/bin`
and the `../../share/pixmaps/yieldsakingwallet128.png` to `/usr/share/pixmaps`

yieldstakingwallet-qt.protocol (KDE)

