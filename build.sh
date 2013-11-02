#!/bin/bash
BASE_DIR=`pwd`
TARGET_OS=`uname -s`
JEMALLOC_PATH="$BASE_DIR/deps/jemalloc-3.4.0"
LIBEVENT_PATH="$BASE_DIR/deps/libevent-2.0.21-stable"
C=gcc
CC=g++

case "$TARGET_OS" in
    Darwin)
        ;;
    Linux)
        PLATFORM_LIBS="-lrt -pthread"
        ;;
    CYGWIN_*)
        ;;
    SunOS)
        PLATFORM_LIBS="-lrt"
        ;;
    FreeBSD)
        PLATFORM_LIBS=""
        ;;
    NetBSD)
        PLATFORM_LIBS=" -lgcc_s"
        ;;
    OpenBSD)
        ;;
    DragonFly)
        PLATFORM_LIBS=""
        ;;
    OS_ANDROID_CROSSCOMPILE)
        ;;
    HP-UX)
        ;;
    *)
        echo "Unknown platform!" >&2
        exit 1
esac


######### build jemalloc #########

if [[ $TARGET_OS == CYGWIN* ]]; then
	echo "not using jemalloc on $TARGET_OS"
else
	echo ""
	DIR=`pwd`
	cd "$JEMALLOC_PATH"
	if [ ! -f Makefile ]; then
		echo "building jemalloc..."
		./configure
		make
		echo "building jemalloc finished"
	fi
	cd "$DIR"
	echo ""
fi


######### build libevent #########

DIR=`pwd`
cd "$LIBEVENT_PATH"
if [ ! -f Makefile ]; then
	./configure
	make
fi
cd "$DIR"


######### generate config.h #########

rm -f config.h
echo "#ifndef ICOMET_CONFIG_H" >> config.h
echo "#define ICOMET_VERSION \"`cat version`\"" >> config.h
if [[ $TARGET_OS == CYGWIN* ]]; then
	:
else
	echo "#include <stdlib.h>" >> config.h
	echo "#include <jemalloc/jemalloc.h>" >> config.h
fi
echo "#endif" >> config.h


######### generate config.mk #########

rm -f config.mk

echo C=$C >> config.mk
echo CC=$CC >> config.mk
echo CFLAGS := >> config.mk
echo CFLAGS += -g -O2 -Wall -Wno-sign-compare >> config.mk
echo CFLAGS += -D__STDC_FORMAT_MACROS >> config.mk
echo CFLAGS += -I \"$LIBEVENT_PATH\" >> config.mk
echo CFLAGS += -I \"$LIBEVENT_PATH/include\" >> config.mk
echo CFLAGS += -I \"$LIBEVENT_PATH/compact\" >> config.mk

echo CLIBS := >> config.mk
echo CLIBS += $PLATFORM_LIBS >> config.mk

if [[ $TARGET_OS == CYGWIN* ]]; then
	:
else
	echo CFLAGS += -I \"$JEMALLOC_PATH/include\" >> config.mk
	echo CLIBS += \"$JEMALLOC_PATH/lib/libjemalloc.a\" >> config.mk
fi

echo LIBEVENT_PATH = $LIBEVENT_PATH >> config.mk

