# Sticky-Mouse-Trap
A simple program for X11 window system that prevents the cursor from crossing into another monitor when you don't want it to. 

Windows has this feature for the corners, while few Linux DEs have it, so working with multiple monitors on Linux can be annoying as I'm used to throwing my mouse into the corners to push buttons all the time. This program adds exactly that - and its behaviour is highly customizable. The default tries to resist cursor movement when you're trying to hit a button in the edge of the screen, while letting the cursor pass (almost) freely when you actually want it to cross into the other screen.

# stick-cursor-to-screen
The name of the repo from my less creative days.

# Configuration editing
The configuration file is `sticky-mouse-trap.cfg`. It should be stored somewhere in the `~/.config/` directory but it's distro-dependant. Launch the program in terminal to find out where the configuration is stored. You can edit the config while the program is running and it should pick up the changes. If it doesn't, save the config again or send the `SIGHUP` signal to the program.

# Building from scratch
Just use CMake to build after installing the dependencies.

# Dependencies
The header-only utilities library `MUtilize` is downloaded automatically by CMake.

The only other dependencies are X11's XInput and Xrandr headers.

* On Ubuntu, they an be found in `libxi` and `librandr` development packages:  
`sudo apt-get install libxi-dev libxrandr-dev`
* On Fedora, the equivalent is  
`sudo dnf install libXi-devel libXrandr-devel`

On other systems find the equivalent packages that hold headers:

```
/usr/include/X11/extensions/XInput.h  
/usr/include/X11/extensions/XInput2.h  
/usr/include/X11/extensions/Xrandr.h  
```
