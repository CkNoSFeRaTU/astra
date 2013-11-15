
SOURCES="module_lua.c module_stream.c crc32b.c"
SOURCES="$SOURCES sha1.c base64.c md5.c strhex.c"
SOURCES="$SOURCES astra.c log.c timer.c utils.c json.c iso8859.c"
MODULES="astra log timer utils json base64 sha1 md5 str2hex"

if [ "$OS" != "mingw" ] ; then
    SOURCES="$SOURCES pidfile.c"
    MODULES="$MODULES pidfile"
fi

getifaddrs_test_c()
{
    cat <<EOF
#include <stdio.h>
#include <ifaddrs.h>
int main(void) {
    struct ifaddrs *ifaddr;
    const int s = getifaddrs(&ifaddr);
    freeifaddrs(ifaddr);
    return s;
}
EOF
}

check_getifaddrs()
{
    getifaddrs_test_c | $APP_C -Werror $CFLAGS $APP_CFLAGS -o /dev/null -x c - >/dev/null 2>&1
}

if check_getifaddrs ; then
    CFLAGS="-DWITH_IFADDRS=1"
else
    echo "$MODULE/module.mk: warning: utils.ifaddrs() is not available" >&2
fi
