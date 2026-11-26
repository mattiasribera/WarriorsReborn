# Top-level targets that span runtime/ and launcher/. Each part keeps its own Makefile.
#
#   make appimage   WarriorsReborn-<version>-x86_64.AppImage (tools/appimage/build.sh; the
#                   version is RELEASE_VERSION, by default the one in runtime/Makefile)

appimage:
	sh tools/appimage/build.sh $(if $(APPIMAGE_OUT),-o $(APPIMAGE_OUT))

.PHONY: appimage
