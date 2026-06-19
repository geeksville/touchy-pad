# User widgets

Adopt that as the standard 'better than streamdeck' replacement.  so people can make screens with guis that do interesting things for any foreground app. 

* toml/json?
* app triggers - skip in layout when indicated app is not foreground
* allow multiple per uscr.  allow multiple uscrs
* python callbacks for behaviors / datasources
* url based fetching/sharing/registry (new? stream controler? opendeck?)
* asset packs?  
* let user pair python handler code with device GUI.  
* Make a few examples (fps widget? steam coverart? perfmon?).  
* Extend those uscrs with triggers of "show when app X is in foreground".  
* add some of the example JSON to the API docs

## ai copypasta

### ksystemstats vs plasmoid
hmm... musings here... probaby no plasmoids at first - ksystemstats dbus queries instead. QQuickRenderControl looks awesome to allow a qt6 api for writing to touchies. see here
https://gemini.google.com/share/091d654412c4 

possibly render kde plasmoids https://store.kde.org/browse?cat=705&tag=plasmoid
https://userbase.kde.org/Plasma/Plasmoids 
https://medium.com/@linuxrootroom/what-is-plasmoid-in-kde-plasma-desktop-939717e498f0 

### steam cover art

Nope. There isn't a magical, all-in-one package that snoops on your active windows and hits up Valve's servers for high-res JPEGs. You're asking for a Frankenstein library that crosses OS-level window management with web API scraping.

But this is Python, so you can easily duct-tape two tools together to do exactly this:

Spot the Game: Use pygetwindow (or win32gui on Windows) to grab the text title of whatever window is currently in the foreground.

Fetch the Data: Feed that window title into a wrapper like python-steam-api to search the Steam store and extract the game's unique AppID.

Once you have the AppID, you don't even need a library to get the art. Steam's content delivery network uses completely predictable URLs. Just plug the ID into this link and grab it with the requests library:

https://steamcdn-a.akamaihd.net/steam/apps/<APP_ID>/library_600x900.jpg

### mangohud

MangoHud doesn't expose a fancy API, D-Bus interface, or a magical JSON socket for you to live-query its metrics. It's a selfish overlay that hooks directly into the Vulkan/OpenGL swapchain and keeps its performance secrets strictly on your screen.

2. Read the Source Yourself
MangoHud doesn't use black magic to get its info. If you want the raw data, bypass the middleman entirely. You can programmatically read /sys/class/drm/ for AMD/Intel stats, use nvidia-smi for Nvidia, and parse lm-sensors for standard hardware metrics.