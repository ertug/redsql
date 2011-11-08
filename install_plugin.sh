#!/bin/bash

SONAME='ha_redis.so'
PLUGIN_DIR='/usr/lib/mysql/plugin/'

rm -v $PLUGIN_DIR$SONAME
echo '-------------------------'
scons || exit 1
echo '-------------------------'
mysql -uroot -e "FLUSH TABLES"
echo 'flushed tables'
sleep 1
echo '-------------------------'
mysql -uroot -e "UNINSTALL PLUGIN redis"
echo 'uninstalled'
echo '-------------------------'
sleep 1
cp -v $SONAME $PLUGIN_DIR || exit 1
echo '-------------------------'
sleep 1
mysql -uroot -e "INSTALL PLUGIN redis SONAME '$SONAME'" || exit 1
echo 'installed'
