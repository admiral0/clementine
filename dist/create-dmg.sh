#!/bin/sh
# author: max@last.fm, muesli@tomahawk-player.org
# brief:  Produces a compressed DMG from a bundle directory
# usage:  Pass the bundle directory as the only parameter
# note:   This script depends on the Tomahawk build system, and must be run from
#         the build directory
################################################################################


#if [ -z $VERSION ]
#then
#    echo VERSION must be set
#    exit 2
#fi

if [ -z "$1" ]
then
    echo "Please pass the bundle.app directory as the first parameter."
    exit 3
fi
################################################################################


NAME=$(basename "$1" | perl -pe 's/(.*).app/\1/')
IN="$1"
TMP="dmg/$NAME"
OUT="$NAME.dmg"
mkdir -p "$TMP"
################################################################################


# clean up
rm -rf "$TMP"
rm -f "$OUT"

# create DMG contents and copy files
mkdir -p "$TMP/.background"
cp ../dist/dmg_background.png "$TMP/.background/background.png"
cp ../dist/DS_Store.in "$TMP/.DS_Store"
chmod go-rwx "$TMP/.DS_Store"
ln -s /Applications "$TMP/Applications"
# copies the prepared bundle into the dir that will become the DMG 
cp -R "$IN" "$TMP"

# create
hdiutil makehybrid -hfs -hfs-volume-name Clementine -hfs-openfolder "$TMP" "$TMP" -o tmp.dmg
hdiutil convert -format UDZO -imagekey zlib-level=9 tmp.dmg -o "$OUT"

# cleanup
rm tmp.dmg

#hdiutil create -srcfolder "$TMP" \
#               -format UDZO -imagekey zlib-level=9 \
#               -scrub \
#               "$OUT" \
#               || die "Error creating DMG :("

# done !
echo 'DMG size:' `du -hs "$OUT" | awk '{print $1}'`
