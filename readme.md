### A port in progress of jamin to Haiku OS

### Dependencies:
```
pkgman install ladspa_sdk_devel fftw_devel gtk3_devel libxml2_devel gettext gettext_devel make automake autoconf libtool intltool pkgconfig glib2_devel

```

### Build / Install libspa
```
https://github.com/swh/ladspa.git
./autogen.sh
./configure --prefix=/boot/home/config/non-packaged
curl -L https://cpanmin.us | perl - --self-upgrade
cpanm List::MoreUtils
make
make install
```

### Build / Test jamin:
```
gcc -shared -o libjack.so jack_stubs.c
LIBRARY_PATH="$LIBRARY_PATH:." ./autogen.sh ACLOCAL_FLAGS="-I /boot/system/develop/headers/m4" JACK_CFLAGS="-I." JACK_LIBS="-L. -ljack"
make CFLAGS="-fcommon -I.." LDFLAGS="-L.. -ljack"

export LIBRARY_PATH="$LIBRARY_PATH:."
export LADSPA_PATH=/boot/home/config/non-packaged/lib/ladspa
./src/jamin -d -t -f test/files/default.jam

```
