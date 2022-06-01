# Sticky-Mouse-Trap
A simple program for X11 window system that prevents the cursor from crossing into another monitor when you don't want it to. 

# stick-cursor-to-screen
The name of the repo from my less creative days.

Windows has this feature for the corners, while few Linux DEs have it, so working with multiple monitors on Linux can be annoying as I'm used to throwing my mouse into the corners to push buttons all the time. This program adds exactly that - and its behaviour is highly customizable. The default tries to resist cursor movement when you're trying to hit a button in the edge of the screen, while letting the cursor pass (almost) freely when you actually want it to cross into the other screen.

# Building from scratch
Just use CMake to build after installing the dependencies.

# Dependencies
The only dependencies are X11's XInput and Xrandr headers. They an be found in `libxi` and `librandr` development packages. On ubuntu-based systems do this:

```
sudo apt-get install libxi-dev
sudo apt-get install libxrandr-dev
```

On other systems find the equivalent packages that hold headers:
```
/usr/include/X11/extensions/XInput.h
/usr/include/X11/extensions/XInput2.h
/usr/include/X11/extensions/Xrandr.h
```