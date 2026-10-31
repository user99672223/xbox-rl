// The xbox-rl desktop's first-start layout (Plasma shell scripting): the wallpaper, and one floating dock at the
// bottom centre that fits its content - launcher, pinned apps with running windows, tray, clock - sized for a TV
// seen from the couch. Runs once, when plasmashell starts with no saved layout.
var desk = desktops();
for (var i = 0; i < desk.length; i++) {
    desk[i].wallpaperPlugin = "org.kde.image";
    desk[i].currentConfigGroup = ["Wallpaper", "org.kde.image", "General"];
    desk[i].writeConfig("Image", "file:///usr/share/wallpapers/xbox-rl/contents/images/3840x2160.svg");
    desk[i].writeConfig("FillMode", "2");   // scaled and cropped
}

var dock = new Panel;
dock.location = "bottom";
dock.height = 2 * Math.ceil(gridUnit * 3.0 / 2);
dock.floating = true;
dock.lengthMode = "fit";
dock.alignment = "center";
dock.hiding = "none";
dock.opacity = "adaptive";

var kickoff = dock.addWidget("org.kde.plasma.kickoff");
kickoff.currentConfigGroup = ["General"];
kickoff.writeConfig("icon", "start-here-kde-symbolic");
kickoff.writeConfig("favoritesDisplay", "0");

dock.addWidget("org.kde.plasma.marginsseparator");

var tasks = dock.addWidget("org.kde.plasma.icontasks");
tasks.currentConfigGroup = ["General"];
tasks.writeConfig("launchers", [
    "applications:org.kde.konsole.desktop",
    "applications:org.kde.dolphin.desktop",
    "applications:firefox-esr.desktop",
    "applications:systemsettings.desktop"
]);
tasks.writeConfig("iconSpacing", "2");
tasks.writeConfig("maxStripes", "1");
tasks.writeConfig("indicateAudioStreams", "false");

dock.addWidget("org.kde.plasma.marginsseparator");

var tray = dock.addWidget("org.kde.plasma.systemtray");

var clock = dock.addWidget("org.kde.plasma.digitalclock");
clock.currentConfigGroup = ["Appearance"];
clock.writeConfig("showDate", "true");
clock.writeConfig("dateFormat", "shortDate");
clock.writeConfig("use24hFormat", "2");
