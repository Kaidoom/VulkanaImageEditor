#!/bin/sh
# KDE runs this once per user through kconf_update, never an RPM root scriptlet.
# Existing Dolphin profiles often save a provider allowlist before Vulkana is
# installed. Add only our provider; never reset that list to KDE's defaults.
set -eu

command -v kreadconfig6 >/dev/null
command -v kwriteconfig6 >/dev/null

missing='__vulkana_no_preview_provider_list__'
plugins=$(kreadconfig6 --file dolphinrc --group PreviewSettings --key Plugins --default "$missing")
if [ "$plugins" = "$missing" ]; then
    # Dolphin also inherits global KDE settings when it has no local entry.
    plugins=$(kreadconfig6 --file kdeglobals --group PreviewSettings --key Plugins --default "$missing")
fi
case "$plugins" in
    "$missing"|'')
        # No list: KDE already enables newly installed providers by default.
        # An explicitly empty list means all providers were disabled; retain it.
        exit 0 ;;
esac
case ",$plugins," in
    *,vulkana,*) exit 0 ;;
esac

# Let KConfig handle escaping, atomic writes and immutable settings. Keep all
# existing providers, including unavailable ones, in their original order.
kwriteconfig6 --file dolphinrc --group PreviewSettings --key Plugins --notify -- "$plugins,vulkana"
