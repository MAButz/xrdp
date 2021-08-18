#THIS ARE MY NOTES OF BUILDING AN INSTALLING XORGXRDP AND XRDP WITH GPU ACCELERATION
#TESTED ON DEBIAN 9.13

#-- Build XorgXrdp with GPU acceleration ("script" - to be adjusted to your needs) : --

## << BUILD AND INSTALL SCRIPT START >> ##
#!/bin/bash
# Install Latest XRDP with XORGXRDP

# README
# Manual steps required first:
# Enable "Non-Free" repost in APT by adding "non-free" at the end of every URL in /etc/apt/sources.list

# Target Versions and Folders
XORGXRDP_VERSION=0.2.16
XRDP_VERSION=0.9.16
BUILD_DIR=/tmp/xrdpbuild

# Prepare Build Directory
rm -f -r $BUILD_DIR
mkdir -p $BUILD_DIR

# Install Dependencies - Dev
apt-get install -y make autoconf libtool intltool pkg-config nasm xserver-xorg-dev libssl-dev libpam0g-dev libjpeg-dev libfuse-dev libopus-dev libmp3lame-dev libxfixes-dev libxrandr-dev libgbm-dev libepoxy-dev libegl1-mesa-dev

# Pulse Audio Dependencies
apt-get install -y libcap-dev libsndfile-dev libsndfile1-dev libspeex-dev libpulse-dev

# Non Free Dependencies
apt-get install -y libfdk-aac-dev

# Install Dependencies - Permanent
apt-get install pulseaudio
apt-get install xserver-xorg 

# Build and Install XRDP
cd $BUILD_DIR
#wget https://github.com/neutrinolabs/xrdp/releases/download/v$XRDP_VERSION/xrdp-$XRDP_VERSION.tar.gz
wget https://github.com/MAButz/xrdp/archive/refs/tags/v$XRDP_VERSION.tar.gz
tar xvzf xrdp-*.tar.gz

rm xrdp-*.tar.gz
cd xrdp-*

./bootstrap
./configure --enable-x264 --enable-glamor --enable-rfxcodec --enable-mp3lame --enable-fdkaac --enable-opus --enable-pixman --enable-fuse --enable-jpeg --enable-tjpeg --enable-ipv6 --enable-painter --enable-vsock

make
make install

ln -s /usr/local/sbin/xrdp{,-sesman} /usr/sbin

echo "XRDP has been installed"

# Build and Install XORGXRDP
cd $BUILD_DIR
wget https://github.com/neutrinolabs/xorgxrdp/releases/download/v$XORGXRDP_VERSION/xorgxrdp-$XORGXRDP_VERSION.tar.gz
tar xvzf xorgxrdp-*.tar.gz

rm xorgxrdp-*.tar.gz
cd xorgxrdp-*

./bootstrap
#./configure --enable-glamor --enable-rfxcodec --enable-mp3lame --enable-fdkaac --enable-opus --enable-pixman --enable-fuse --enable-jpeg --enable-ipv6
./configure --enable-x264 --enable-glamor --enable-rfxcodec --enable-mp3lame --enable-fdkaac --enable-opus --enable-pixman --enable-fuse --enable-jpeg --enable-tjpeg --enable-ipv6 --enable-painter --enable-vsock
make
make install

echo "XORGXRDP has been installed"

# Build and Install Pulseaudio-Sink
pulseaudio --version # Show installed version from here and use that version of pulseaudio

cd $BUILD_DIR
PULSEAUDIO_VERSION=$(pulseaudio --version | awk '{print $2}')
wget https://freedesktop.org/software/pulseaudio/releases/pulseaudio-$PULSEAUDIO_VERSION.tar.xz
tar xf pulseaudio-$PULSEAUDIO_VERSION.tar.xz
rm pulseaudio-$PULSEAUDIO_VERSION.tar.xz

cd pulseaudio-$PULSEAUDIO_VERSION/
./configure # --with-speex # Can also be with speex
PULSEAUDIO_SRC_DIR="$PWD"

cd $BUILD_DIR
wget https://github.com/neutrinolabs/pulseaudio-module-xrdp/archive/v0.4.tar.gz -O pulseaudio-module-xrdp.tar.gz
tar xvzf pulseaudio-module-xrdp.tar.gz
rm pulseaudio-module-xrdp.tar.gz

cd pulseaudio-module-xrdp*
./bootstrap  
./configure PULSE_DIR=$PULSEAUDIO_SRC_DIR
make
make install

ls $(pkg-config --variable=modlibexecdir libpulse)

# Cleanup
cd /tmp
rm -f -r $BUILD_DIR

# Remove Pulse Audio Dependencies
apt-get remove -y --purge libcap-dev libsndfile-dev libsndfile1-dev libspeex-dev libpulse-dev

# Remove Non Free Dependencies
apt-get remove -y --purge libfdk-aac-dev

# Remove Build Dependencies
apt-get remove -y --purge make autoconf libtool intltool pkg-config nasm xserver-xorg-dev libssl-dev libpam0g-dev libjpeg-dev libfuse-dev libopus-dev libmp3lame-dev libxfixes-dev libxrandr-dev libgbm-dev libepoxy-dev libegl1-mesa-dev

# Clean Residues
apt autoremove -y
apt clean

# Enable Service
systemctl enable xrdp
systemctl start xrdp

# Script Completed
#exit


## << BUILD AND INSTALL SCRIPT END >> ##

#-- Extra notes: --

# Create GPU Passtrough Nodes on Guest:

# /etc/start/init.sh
# crontab -e
# @reboot   /etc/start/init.sh

#!/bin/sh
# GPU Pass Trough
#mkdir -p /dev/dri
#mknod -m 666 /dev/dri/card0 c 226 0
#mknod -m 666 /dev/dri/renderD128 c 226 128
#mknod -m 666 /dev/fb0 c 29 0


#- Enable Non Root Login: -
#nano /etc/X11/Xwrapper.config

# CHANGE
#allowed_users=console
# TO
#allowed_users=anybody


#- Set specific GUI session: -
# In Home folder of user:
#echo mate-session > ./.xsession
#echo openbox-session  > ./.xsession
#echo gnome-session --session=ubuntu-2d  > ./.xsession
#echo xfce4-session  > ./.xsession