# User widgets

Adopt that as the standard 'better than streamdeck' replacement.  so people can make screens with guis that do interesting things for any foreground app. 

* app triggers - skip in layout when indicated app is not foreground
* allow multiple per uscr.  allow multiple uscrs
* python callbacks for behaviors / datasources
* url based fetching/sharing/registry (new? stream controler? opendeck?)
* asset packs?  
* let user pair python handler code with device GUI.  
* Make a few examples (fps widget? steam coverart? perfmon?).  
* Extend those uscrs with triggers of "show when app X is in foreground".  
* add some of the example JSON to the API docs

* Possibly esphome yaml https://esphome.io/components/lvgl/ and https://esphome.io/components/image/#display-image 

## dynamic rendering

* extend device platform info to include a temp_is_flash bool.  Most devices should have that as false.  But on devices without PSRAM they should set that flag true.
* make the image() and image_button() functions so that "asset" paramter can be a PIL Image object instead of just a string.
* put the file in T:images/N.bin. Where N is a unique number starting at 1 for each new run of the API client.  Device/simulator should usually map T: ('temp') to a ramdisk or (for devices that only have tiny ramdisks) F:tmp/FOO.
* fixme: somehow have it also accept a generator that can be used to make new images either periodically or on demand.  What is a clean pythonic api for this?  Ideally the 'on demand' api would be used (by adding a timer instance) to implement the 'peroidic updated' api.
* if we generate a new image rewrite the file on device, which will implicitly force a redraw of that component

### example of how existing usage works
```python
        image_button(
            "smile",
            asset="F:host/images/smiley.png",
            on_click=host_action(on_event=lambda e: logger.info("[smile]  widget=%r", e.user_data)),
        )
```

### example of using pillow for rendering text strings with rounded rect:
```python
from PIL import Image, ImageDraw

img = Image.new('RGB', (200, 100), color='black')
draw = ImageDraw.Draw(img)

# A one-liner, as the coding gods intended.
draw.rounded_rectangle([10, 10, 190, 90], radius=15, fill='blue')
draw.text((65, 45), "hello world", fill='white')

# img.show() or img.save() to prove it happened
```

### example of using weasyprint to render HTML as a pixel buf
Someday we might want to support this

```python
from weasyprint import HTML
import io
from PIL import Image

# Render HTML string directly to PNG bytes in memory
png_bytes = HTML(string="<h1>Hello <span style='color:blue;'>World</span></h1>").write_png()

# Load those bytes into a Pillow image
img = Image.open(io.BytesIO(png_bytes))
img.show()
```

## ai copypasta
misc notes on ideas...

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