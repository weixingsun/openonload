#!/bin/sh

# Find the libc which is really used.  Usually, it is one of
# /lib/libc.so.6, /lib32/libc.so.6, /lib64/libc.so.6.
# Debian-derivative name conventions differ from other linuxes.
# So, we do not try to guess and ask the system.
find_libc()
{
    local dir=$(mktemp -d)
    cat >$dir/hello.c <<EOF
int main(int argc, char ** argv) {return 0;}
EOF
    $CC $CFLAGS $dir/hello.c -o $dir/hello >/dev/null
    ldd $dir/hello |egrep '^[[:space:]]*libc.so.6[[:space:]]' \
        |awk '{print $3}'
    rm -rf $dir
}

# Check if the library has the given symbol.
find_sym()
{
    local lib="$1"
    local sym="$2"
    local header="$3"

    echo -n "#define CI_LIBC_HAS_$sym " >>$header
    if nm -D "$lib" |grep -q "$sym"; then
        echo "1" >>$header
    else
        echo "0" >>$header
    fi
}

# Check if the socket.h header has the given symbol.
find_sym_in_socket_h()
{
    local sym="$1"
    local header="$2"

    echo -n "#define CI_LIBC_HAS_$sym " >>$header
    if grep -q "$sym" /usr/include/bits/socket.h; then
        echo "1" >>$header
    else
        echo "0" >>$header
    fi
}

libc_path=$(find_libc)
header="$1"

cat >"$header" <<EOF
/* This header is generated by scripts/libc_compat.sh */
EOF

for sym in __read_chk __recv_chk __recvfrom_chk \
           accept4 pipe2 epoll_pwait ppoll; do
    find_sym "$libc_path" "$sym" "$header"
done

for sym in sendmmsg; do
    find_sym_in_socket_h "$sym" "$header"
done

echo -n "#define CI_HAVE_PCAP " >>$header
check_library_presence pcap.h pcap >>$header

