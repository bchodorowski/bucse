#!/bin/sh

randhex() { head -c 20 /dev/urandom | od -An -tx1 | tr -d ' \n'; }
archive="$(randhex).tar"
tmplist="$(mktemp)"

find . -maxdepth 1 -type f ! -name '*.tar' -print0 > "$tmplist" \
	&& tar --null -T "$tmplist" -cf "$archive" \
	&& sync "$archive" \
	&& xargs -0 rm -- < "$tmplist" \
	&& rm -f "$tmplist"

echo "Created archive: $archive"
