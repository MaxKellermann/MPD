#!/bin/sh

# This script need the following modification in ANDROID_NDK in order to attach
# to the good :main pid
#--- a/prebuilt/linux-x86_64/bin/ndk-gdb.py
#+++ b/prebuilt/linux-x86_64/bin/ndk-gdb.py
#@@ -669,7 +669,7 @@
#             log("Sleeping for {} seconds.".format(args.delay))
#             time.sleep(args.delay)
#
#-    pids = gdbrunner.get_pids(device, pkg_name)
#+    pids = gdbrunner.get_pids(device, pkg_name + ":main")
#     if len(pids) == 0:
#         error("Failed to find running process '{}'".format(pkg_name))
#     if len(pids) > 1:

SCRIPT_PATH=$(dirname $0)
BUILD_PATH="`pwd`"
TMP_PATH="$BUILD_PATH/gdb"
NDK_GDB_ARGS="--force"
ANDROID_NDK=$1

if [ ! -f $ANDROID_NDK/source.properties ];then
    echo "usage: $0 ANDROID_NDK"
    exit 1
fi

if [ ! -f $BUILD_PATH/libmpd.so ];then
    echo "This script need to be executed from the android build directory"
    exit 1
fi

rm -rf "$TMP_PATH"
mkdir -p "$TMP_PATH"

ANDROID_MANIFEST="$SCRIPT_PATH/AndroidManifest.xml"
ABI=`ls "$BUILD_PATH/android/apk/apk/lib" --sort=time | head -n 1`

if [ ! -f "$ANDROID_MANIFEST" -o "$ABI" = "" ]; then
    echo "Invalid manifest/ABI, did you try building first ?"
    exit 1
fi

mkdir -p "$TMP_PATH"/jni
touch "$TMP_PATH"/jni/Android.mk
echo "APP_ABI := $ABI" > "$TMP_PATH"/jni/Application.mk

DEST=obj/local/$ABI
mkdir -p "$TMP_PATH/$DEST"

cp "$BUILD_PATH/libmpd.so" "$TMP_PATH/$DEST"
cp "$ANDROID_MANIFEST" "$TMP_PATH"

(cd "$TMP_PATH" && bash $ANDROID_NDK/ndk-gdb $NDK_GDB_ARGS)
