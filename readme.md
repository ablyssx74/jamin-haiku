### A port in progress of jamin to Haiku OS

Requires:
```pkgman install ladspa_sdk_devel gtk3_devel gettext_devel automake autoconf libtool intltool pkgconfig glib2_devel```


Testing:
```
ln -s ${PWD}/libjack.so ~/config/non-packaged/add-ons/ladspa/fast_lookahead_limiter_1913.so
ln -s ${PWD}/libjack.so ~/config/non-packaged/add-ons/ladspa/foo_limiter.so
ln -s ${PWD}/libjack.so ~/config/non-packaged/add-ons/ladspa/sc4_1882.so

export LIBRARY_PATH="$LIBRARY_PATH:."
export LADSPA_PATH=~/config/non-packaged/add-ons/ladspa
./src/jamin -d -t -f test/files/default.jam
```
