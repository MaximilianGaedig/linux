# Remove -Werror from all makefiles
makefiles="$(find "." -type f -name Makefile)
	$(find "." -type f -name Makefile.common)
	$(find "." -type f -name Kbuild)"

echo $makefiles
for i in $makefiles; do
	grep Werror "$i"
	# sed -i 's/-Werror-/-W/g' "$i"
	# sed -i 's/-Werror=/-W/g' "$i"
	# sed -i 's/-Werror//g' "$i"
done

