builddir="."

# Remove -Werror from all makefiles
makefiles="$(find "$builddir" -type f -name Makefile)
	$(find "$builddir" -type f -name Makefile.common)
	$(find "$builddir" -type f -name Kbuild)"
for i in $makefiles; do
	sed -i 's/-Werror-/-W/g' "$i"
	sed -i 's/-Werror=/-W/g' "$i"
	sed -i 's/-Werror//g' "$i"
done
