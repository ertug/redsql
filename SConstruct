MYSQL_PATH =  'ext/mysql-5.1/'
HIREDIS_PATH = 'ext/hiredis/'

hiredis = [HIREDIS_PATH + 'net.c',
           HIREDIS_PATH + 'hiredis.c',
           HIREDIS_PATH + 'sds.c']

src = ['src/util.cc',
       'src/redis.cc',
       'src/ha_redis.cc']

SharedLibrary('ha_redis.so', hiredis + src,
              CPPPATH = [MYSQL_PATH + 'builddir/include',
                         MYSQL_PATH + 'include',
                         MYSQL_PATH + 'sql',
                         MYSQL_PATH + 'regex',
                         MYSQL_PATH + 'strings',
                         HIREDIS_PATH],
              CCFLAGS='-shared -fno-rtti -DMYSQL_DYNAMIC_PLUGIN -g -O2 -fno-implicit-templates -fno-exceptions',
              SHLIBPREFIX='')
#-DWITH_DEBUG=1 -O2 -felide-constructors -fno-exceptions
#-DMYSQL_DYNAMIC_PLUGIN -prefer-non-pic -g  -DSAFE_MUTEX -O3 -DBIG_JOINS=1  -fno-strict-aliasing   -DUNIV_LINUX
