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
make clean
./configure JACK_CFLAGS="-I." JACK_LIBS="-L$(pwd) -ljack"
make CFLAGS="-fcommon -I." LDFLAGS="-L$(pwd) -ljack"

g++ -shared -fPIC jack_stubs.cpp -o src/libjack.so -lmedia -lbe

export LIBRARY_PATH=".:$LIBRARY_PATH"
export LADSPA_PATH=/boot/home/config/non-packaged/lib/ladspa
./src/jamin -f test/files/default.jam

```
