#include <MiIni.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>
#include <circular_queue.h>
#include <linux/limits.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <vector>

using namespace std::chrono;

struct PtrEntry {
    PtrEntry(int x, int y, float speed, float dx, float dy)
        : x(x), y(y), speed(speed), dx(dx), dy(dy) {
        moveTimepoint = high_resolution_clock::now();
    }

    int x, y;
    float dx, dy;
    float speed;
    time_point<high_resolution_clock> moveTimepoint;
};

struct PassConfig {
    bool always;
    duration<float> maxDelay, minDelay, baseDelay;
    duration<float> returnBefore;
};

/*
Config variables
*/
std::string cfgPath;
bool cfgSavedByMyself;
MiIni<std::string> config;
bool cfgEnabled;
int cfgPtrInputsToRemember;
duration<float> cfgPtrRememberForSeconds;
float cfgResistanceSlowdownExponent;
float cfgResistanceSpeedupExponent;
float cfgResistanceConstSpeedExponent;
float cfgResistanceDirectionExponent;
float cfgPassthroughSmoothingFactor;
PassConfig cfgEdgePass, cfgCornerPass;
float cfgCornerSizeFactor;
int cfgResistanceMargins;

/*
Config monitor variables
*/
int inotifyFD;
int inotifyCfgW;

/*
Display variables
*/
struct Monitor {
    int x, y;
    unsigned int w, h;
    Window inputWindow;

    bool contains(int xpos, int ypos, int margin = 0) {
        return (xpos >= x + margin && xpos < (x + w - margin) && ypos >= y + margin &&
                ypos < (y + h - margin));
    }

    void snapPosition(int *xpos, int *ypos) {
        if (*xpos < x + cfgResistanceMargins)
            *xpos = x + cfgResistanceMargins;
        if (*ypos < y + cfgResistanceMargins)
            *ypos = y + cfgResistanceMargins;
        if (*xpos > x + w - cfgResistanceMargins - 1)
            *xpos = x + w - cfgResistanceMargins - 1;
        if (*ypos > y + h - cfgResistanceMargins - 1)
            *ypos = y + h - cfgResistanceMargins - 1;
    }
};

Display *display;  // our display
Window rootWindow; // root wnd of our display
std::vector<Monitor> monitors;
Monitor *currentMonitor; // the monitor in which the pointer is

/*
Resistance calculation variables
*/
circular_queue<PtrEntry> ptrMemory; // ptr positions and data
bool onEdge;                        // are we on edge rn
PassConfig *lastPassCfg;
time_point<high_resolution_clock> touchedEdgeTime; // the time point when we touched the edge, to
                                                   // detect delay
time_point<high_resolution_clock>
    brokeFromTimepoint; // the time point when we last broke from a monitor
Time lastPtrMoveX11Time;
Monitor *brokeFromMonitor; // the monitor we passed FROM last time. Useful for
                           // returning to previous monitor when we miss a
                           // button or smthng.

std::string getDefaultConfigPath() {
    /*
    By default, the config file will be searched for in the directory
    specified by the XDG Base Directory Specification (XDG_CONFIG_HOME variable)
       <http://standards.freedesktop.org/basedir-spec/basedir-spec-latest.html>.
    If XDG_CONFIG_HOME is not set, search directly in HOME/.config/.
    If HOME isn't set either, use the structure from getpwuid() to get the home
    directory and then search in the .config directory.
    */
    std::string cfgPath;
    char *env;
    if ((env = getenv("XDG_CONFIG_HOME")) != nullptr && env[0] != '\0')
        cfgPath = std::string(env) + "/sticky-mouse-trap.cfg";
    else if ((env = getenv("XDG_CONFIG_DIRS")) != nullptr && env[0] != '\0') {
        cfgPath = std::string(env);
        cfgPath = cfgPath.substr(0, cfgPath.find_first_of(':')) + "/sticky-mouse-trap.cfg";
    } else if ((env = getenv("HOME")) != nullptr && env[0] != '\0')
        cfgPath = std::string(env) + "/.config/sticky-mouse-trap.cfg";
    else {
        auto pwd = getpwuid(getuid());
        cfgPath = std::string(pwd->pw_dir) + "/.config/sticky-mouse-trap.cfg";
    }

    return cfgPath;
}

void loadConfig() {
    if (cfgPath == "") {
        cfgPath = getDefaultConfigPath();
    }

    std::cout << "Loading config " << cfgPath << std::endl;

    // Remove old watch (to prevent 'infinite loop' of config reloading)
    if (inotifyCfgW != -1)
        inotify_rm_watch(inotifyFD, inotifyCfgW);

    try {
        config.open(cfgPath, false);

        cfgEnabled = config.get("General", "Enabled", true);

        cfgCornerSizeFactor = config.get("Screen", "CornerSizeFactor", 0.1f);
        cfgResistanceMargins = config.get("Screen", "ResistanceMargins", 1);

        cfgEdgePass.always = config.get("Edge Passthrough", "AllowAlways", false);
        cfgEdgePass.baseDelay =
            (duration<float>)config.get("Edge Passthrough", "BaseDelayOfSeconds", 0.4);
        cfgEdgePass.maxDelay =
            (duration<float>)config.get("Edge Passthrough", "MaxDelayOfSeconds", 0.6);
        cfgEdgePass.minDelay =
            (duration<float>)config.get("Edge Passthrough", "MinDelayOfSeconds", 0.0);
        cfgEdgePass.returnBefore =
            (duration<float>)config.get("Edge Passthrough", "FreelyReturnBeforeSeconds", 1);

        cfgCornerPass.always = config.get("Corner Passthrough", "AllowAlways", false);
        cfgCornerPass.baseDelay =
            (duration<float>)config.get("Corner Passthrough", "BaseDelayOfSeconds", 0.7);
        cfgCornerPass.maxDelay =
            (duration<float>)config.get("Corner Passthrough", "MaxDelayOfSeconds", 1);
        cfgCornerPass.minDelay =
            (duration<float>)config.get("Corner Passthrough", "MinDelayOfSeconds", 0.0);
        cfgCornerPass.returnBefore =
            (duration<float>)config.get("Corner Passthrough", "FreelyReturnBeforeSeconds", 1);

        cfgPtrInputsToRemember = config.get("Movement Calculation", "NoInputsToRemember", 50);
        cfgPtrRememberForSeconds =
            (duration<float>)config.get("Movement Calculation", "RememberForSeconds", 0.15);
        cfgResistanceSlowdownExponent =
            config.get("Movement Calculation", "ResistanceSlowdownExponent", 4.0);
        cfgResistanceSpeedupExponent =
            config.get("Movement Calculation", "ResistanceSpeedupExponent", 1.0);
        cfgResistanceConstSpeedExponent =
            config.get("Movement Calculation", "ResistanceConstantSpeedExponent", 0.1);
        cfgResistanceDirectionExponent =
            config.get("Movement Calculation", "ResistanceByDirectionExponent", 1.0);
        cfgPassthroughSmoothingFactor =
            config.get("Movement Calculation", "PassthroughSmoothingFactor", 0.05);
    } catch (const MiIni<>::FileError &e) {
        std::cerr << "Error while reading configuration: " << e.what() << '\n';
    }

    config.sync();           // In case the config didn't exist before
    cfgSavedByMyself = true; // Needed to skip the file change notification

    // Add config modification watch
    if (inotifyFD != -1) {
        inotifyCfgW = inotify_add_watch(inotifyFD, cfgPath.c_str(), IN_CLOSE_WRITE);

        if (inotifyCfgW == -1)
            std::cerr << "Error in inotify_add_watch(). Config '" << cfgPath
                      << "' will not be auto-reloaded when changed." << std::endl;
    }
}

Window createMonitorSpanWindow(int x, int y, unsigned int w, unsigned int h) {
    XSetWindowAttributes atr;
    atr.override_redirect = true;
    Window wnd = XCreateWindow(display, rootWindow, x, y, w, h,
                               0,                  // border width
                               0,                  // depth
                               InputOnly,          // class (input-only)
                               0,                  // visual
                               CWOverrideRedirect, // valuemask
                               &atr                // attributes
    );

    /*In case the window manager still interferes, make the window fullscreen*/
    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", true);
    Atom wm_fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", true);
    XChangeProperty(display, wnd, wm_state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&wm_fullscreen, 1);

    /* Keep the window on the bottom so it's not visible or interactible when shown*/
    XLowerWindow(display, wnd);

    /*In case this doesn't work due to the window manager, tell the WM to treat the window as a
     * desktop surface. This shouldn't be an issue since this window won't be shown most of the
     * time*/
    Atom wm_window_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    Atom wm_desktop = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(display, wnd, wm_window_type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&wm_desktop, 1);

    return wnd;
}

Window getWindowAt(Window parent, int x, int y) {
    unsigned int nchildren;
    Window retroot, retparent, *children;
    Window child;
    XWindowAttributes wndAtr;

    Window parentDummy;
    int root_x, root_y, win_x, win_y;
    unsigned int maskDummy;
    bool ret = XQueryPointer(display, parent, &parentDummy, &child, &root_x, &root_y, &win_x,
                             &win_y, &maskDummy);

    if (ret) {
        printf("Child %x; ", (int)child);
        if (child == 0 || child == parent) {
            return parent;
        }
        child = getWindowAt(child, x, y);
        if (child == 0) {
            return parent;
        }
        return child;
    }

    return 0;
}

Monitor *getMonitorAt(int x, int y) {
    Monitor *monitor = nullptr;
    for (auto &m : monitors) {
        if (m.contains(x, y)) {
            monitor = &m;
        }
    }
    return monitor;
}

void updateMonitorList() {
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(display, rootWindow);

    for (Monitor &mon : monitors) {
        XDestroyWindow(display, mon.inputWindow);
    }
    monitors.clear();

    // CRTC seems to be a monitor assigned to a rectangle of this Screen
    for (int j = 0; j < res->ncrtc; j++) {
        XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(display, res, res->crtcs[j]);
        if (crtc_info->noutput) {
            monitors.push_back(
                Monitor{crtc_info->x, crtc_info->y, crtc_info->width, crtc_info->height,
                        createMonitorSpanWindow(crtc_info->x + cfgResistanceMargins,
                                                crtc_info->y + cfgResistanceMargins,
                                                crtc_info->width - cfgResistanceMargins * 2,
                                                crtc_info->height - cfgResistanceMargins * 2)});
            printf("Found monitor:%3i x:%5i y:%5i w:%4i h:%4i, Window %x\n", j, crtc_info->x,
                   crtc_info->y, crtc_info->width, crtc_info->height,
                   (int)monitors.back().inputWindow);
        }
        XFree(crtc_info);
    }
    XFree(res);

    // Reset pointer position info
    Window childDummy, parentDummy;
    int root_x, root_y, win_x, win_y;
    unsigned int maskDummy;
    XQueryPointer(display, rootWindow, &rootWindow, &childDummy, &root_x, &root_y, &win_x, &win_y,
                  &maskDummy);

    ptrMemory.clear();
    for (int i = 0; i < cfgPtrInputsToRemember; i++)
        ptrMemory.emplace_back(root_x, root_y, 0, 0, 0);

    // Find the monitor on which we are rn
    currentMonitor = getMonitorAt(root_x, root_y);
    brokeFromMonitor = nullptr;
    onEdge = false;
}

void movePointer(int x, int y) {
    XWarpPointer(display, None, rootWindow, 0, 0, 0, 0, x, y);
    XFlush(display);
}

Window pointerConfined = 0;
void confinePointer(const Monitor *mon) {
    if (pointerConfined == 0) {

        // show the (invisible) window so it can grab the pointer
        XMapWindow(display, mon->inputWindow);

        // use the window server to forcefully keep the pointer in the screen,
        // to prevent flicker
        XGrabPointer(display, mon->inputWindow, false,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                     GrabModeAsync, mon->inputWindow, None, lastPtrMoveX11Time);

        pointerConfined = mon->inputWindow;
        printf("Confined pointer to x:%5i y:%5i w:%4i h:%4i, Window %x\n", mon->x, mon->y, mon->w,
               mon->h, (int)mon->inputWindow);
    }

    // warp the pointer back into the screen just in case
    Window childWnd;
    int x, y, win_x, win_y;
    unsigned int maskDummy;
    XQueryPointer(display, rootWindow, &rootWindow, &childWnd, &x, &y, &win_x, &win_y, &maskDummy);
    currentMonitor->snapPosition(&x, &y);
    movePointer(x, y);

    XFlush(display);
}
void unconfinePointer() {
    if (pointerConfined != 0) {
        XUngrabPointer(display, lastPtrMoveX11Time);
        XUnmapWindow(display, pointerConfined);
        XAllowEvents(display, ReplayPointer, lastPtrMoveX11Time);
        XFlush(display);
        pointerConfined = 0;
        std::cout << "Unconfined pointer" << std::endl;
    }
}

float ptrSpeed1 = 0.0f, ptrSpeed2 = 0.0f;
void pointerSpeedChanged(Time time, int x, int y, double dx, double dy) {
    // store time
    lastPtrMoveX11Time = time;

    // Remember the state
    PtrEntry &prev = ptrMemory[ptrMemory.size() - 1]; // previous pointer state
    PtrEntry current(x, y, 0.0f, dx, dy);             // current pointer state
    duration<float> secondsElapsed = current.moveTimepoint - prev.moveTimepoint;

    // calc speed
    float dis = std::sqrt(dx * dx + dy * dy);
    if (secondsElapsed.count() != 0) {
        current.speed = dis;
        // dis / secondsElapsed.count();
    }
    ptrMemory.pop_front();        // remove old pointer data
    ptrMemory.push_back(current); // add new pointer data

    // Calc 2 average speeds to determine if we are accelerating or slowing
    // down, and use the difference in further calcs
    ptrSpeed1 = 0.0f;
    ptrSpeed2 = 0.0f;

    for (int i = 0; i < ptrMemory.size() - 1; i++) {
        auto timeDiff = current.moveTimepoint - ptrMemory[i].moveTimepoint;

        if (timeDiff <= cfgPtrRememberForSeconds) {
            ptrSpeed1 = ptrMemory[i].speed;
            break;
        }
    }
    ptrSpeed2 = current.speed;

    // printf("Current: %9.2f, first: %9.2f, second: %9.2f, Res: %4.2f\n", dis,
    // ptrSpeed1, ptrSpeed2, resistanceFactor);
}

void pointerPositionChanged(Time time, int x, int y) {

    // Do nothing if we are outside any monitor
    if (currentMonitor) {
        const auto &current = ptrMemory.back();

        // If the pointer tries to exit the monitor
        if (!currentMonitor->contains(x, y, cfgResistanceMargins)) {
            Monitor *newMonitor =
                getMonitorAt(x + cfgResistanceMargins *
                                     ((x > currentMonitor->x + currentMonitor->w / 2) ? 1 : -1),
                             y + cfgResistanceMargins *
                                     ((y > currentMonitor->y + currentMonitor->h / 2) ? 1 : -1));
            PassConfig *passCfg;
            bool pass;

            // Find on which corner/edge we are rn
            bool onHorCorner =
                (x < currentMonitor->x + currentMonitor->w * cfgCornerSizeFactor) ||
                (x > currentMonitor->x + currentMonitor->w * (1.0f - cfgCornerSizeFactor));
            bool onVerCorner =
                (y < currentMonitor->y + currentMonitor->h * cfgCornerSizeFactor) ||
                (y > currentMonitor->y + currentMonitor->h * (1.0f - cfgCornerSizeFactor));
            bool onVerEdge = (y >= currentMonitor->y && y < currentMonitor->y + currentMonitor->h);
            bool onHorEdge = (x >= currentMonitor->x && x < currentMonitor->x + currentMonitor->w);

            if (onHorCorner && onVerCorner)
                passCfg = &cfgCornerPass;
            else
                passCfg = &cfgEdgePass;

            // Should we ignore the resistance altogether?
            if (passCfg->always ||
                (newMonitor == brokeFromMonitor &&
                 (current.moveTimepoint - brokeFromTimepoint) < passCfg->returnBefore)) {
                pass = true;
            } else {
                // keep track of the time if we collided with the edge right now
                if (!onEdge || passCfg != lastPassCfg) {
                    onEdge = true;
                    touchedEdgeTime = current.moveTimepoint;
                }

                // Calc resistance factor for making it harder to pass
                float resistanceFactor;
                if (ptrSpeed1 > 0 && ptrSpeed2 > 0) {
                    // If we are slowing down, resistance must be higher (prolly
                    // trying to hit a button)
                    resistanceFactor = ptrSpeed1 / ptrSpeed2;

                    if (ptrSpeed1 > ptrSpeed2)
                        resistanceFactor =
                            std::pow(resistanceFactor, cfgResistanceSlowdownExponent);
                    else
                        resistanceFactor = std::pow(resistanceFactor, cfgResistanceSpeedupExponent);

                    resistanceFactor *=
                        std::pow(std::abs(ptrSpeed1 - ptrSpeed2) / std::max(ptrSpeed1, ptrSpeed2),
                                 cfgResistanceConstSpeedExponent);

                    if (onVerEdge && current.dx != 0.0)
                        resistanceFactor *= std::pow(ptrSpeed2 / std::abs(current.dx),
                                                     cfgResistanceDirectionExponent);
                    else if (onHorEdge && current.dy != 0.0)
                        resistanceFactor *= std::pow(ptrSpeed2 / std::abs(current.dy),
                                                     cfgResistanceDirectionExponent);
                } else {
                    resistanceFactor = 1;
                }
                resistanceFactor = (resistanceFactor - cfgPassthroughSmoothingFactor) /
                                   (1.0 - cfgPassthroughSmoothingFactor);

                // adjust the base delay by the factor
                auto adjustedDelay =
                    (std::max(std::min(passCfg->baseDelay * resistanceFactor, passCfg->maxDelay),
                              passCfg->minDelay));

                // check how long have we been pushing through the edge and
                // passthrough if it's longer than the expected delay
                if ((current.moveTimepoint - touchedEdgeTime) > adjustedDelay) {
                    pass = true;
                } else {
                    pass = false;
                }
            }
            lastPassCfg = passCfg;

            if (pass) {
                onEdge = false;
                brokeFromTimepoint = current.moveTimepoint;
                brokeFromMonitor = currentMonitor;
                currentMonitor = newMonitor;
            } else {
                /*
                Manually setting the position causes the pointer to 'flicker'
                because of the delay between the movePointer() call and actual
                pointer update on screen. We confine the pointer in window
                spanning the whole monitor.
                */
                confinePointer(currentMonitor);
            }
        } else {
            if (currentMonitor->contains(x + current.dx, y + current.dy, cfgResistanceMargins)) {
                unconfinePointer();
            }
            if (currentMonitor->contains(x, y, cfgResistanceMargins + 1)) {
                onEdge = false;
            }
        }
    } else {
        currentMonitor = getMonitorAt(x, y);
    }
}

/*
SIGNAL HANDLERS
*/
bool reloadCfg;
bool running;
void reloadCfgSignal(int) { reloadCfg = true; }
void terminateSignal(int) { running = false; }

/*
ERROR handlers
*/
int handleXError(Display *d, XErrorEvent *e) {
    char errMsgBuffer[1000];
    XGetErrorText(display, e->error_code, errMsgBuffer, sizeof(errMsgBuffer));
    std::cerr << "Error code: " << e->error_code << ", detail: " << errMsgBuffer << std::endl;
    return 0;
}

int main(int argc, char **argv) {
    // ---Read arguments---
    if (argc >= 2)
        cfgPath = argv[1];

    // ---Prepare inotify---
    inotifyFD = inotify_init();
    if (inotifyFD == -1)
        std::cerr << "Error in inotify_init(). Config will not be "
                     "auto-reloaded when changed."
                  << std::endl;

    inotifyCfgW = -1; // init value for no watch

    const int inotifyBufSize = sizeof(inotify_event) + PATH_MAX + 1;
    char inotifyBuf[inotifyBufSize];
    pollfd inotifyPollFD; // should be an array, but we only need one
    inotifyPollFD.fd = inotifyFD;
    inotifyPollFD.events = POLLIN;

    // ---Load config---
    loadConfig();

    // ---Get display---
    if ((display = XOpenDisplay(NULL)) == NULL) {
        std::cerr << "Cannot open Display! Exiting..." << std::endl;
        return -1;
    }
    XSetErrorHandler(handleXError);

    rootWindow = XDefaultRootWindow(display);
    XAllowEvents(display, AsyncBoth, CurrentTime);

    // ---Load the extension---
    int xiExtOpcode;

    int ev, err;
    if (!XQueryExtension(display, "XInputExtension", &xiExtOpcode, &ev, &err)) {
        std::cerr << "XInput extension is not available. Required to run "
                     "sticky-cursor-screen-edges."
                  << std::endl;
        return -1;
    }

    // ---Check the version---
    int major_op = 2;
    int minor_op = 2;
    int result = XIQueryVersion(display, &major_op, &minor_op);
    if (result == BadRequest) {
        std::cerr << "Required version of XInput is not supported." << std::endl;
        return -1;
    } else if (result != Success) {
        std::cerr << "Couldn't check version of XInput" << std::endl;
        return -1;
    }

    // ---Select XI events---
    XIEventMask masks[1];
    unsigned char mask[(XI_LASTEVENT + 7) / 8];

    memset(mask, 0, sizeof(mask));
    XISetMask(mask, XI_RawMotion);

    masks[0].deviceid = XIAllMasterDevices;
    masks[0].mask_len = sizeof(mask);
    masks[0].mask = mask;

    XISelectEvents(display, DefaultRootWindow(display), masks, 1);
    XFlush(display);

    // ---Monitor list---
    updateMonitorList();
    // notify of resolution changes
    XSelectInput(display, rootWindow, StructureNotifyMask);

    // ---Signal handlers---
    running = true;
    reloadCfg = false;
    signal(SIGHUP, reloadCfgSignal);
    signal(SIGTERM, terminateSignal);

    // ---Event loop---
    XEvent xevent;
    while (running) {
        // After first load, only used on SIGHUP signal or file change
        if (reloadCfg) {
            reloadCfg = false;
            std::cout << "Received signal for reloading config..." << std::endl;
            loadConfig();
        }

        // Check for config change
        bool cfgChanged = false;

        if (poll(&inotifyPollFD, 1, 0) > 0 && inotifyPollFD.revents == POLLIN) {
            int numRead = read(inotifyFD, &inotifyBuf, inotifyBufSize);
            int pos = 0;
            while (pos < numRead) {
                inotify_event *ev = (inotify_event *)(&inotifyBuf + pos);
                if (ev->wd == inotifyCfgW && !cfgSavedByMyself) { // File has been changed
                    cfgChanged = true;
                }
                pos += sizeof(inotify_event) + ev->len;
            }
            cfgSavedByMyself = false;
        }

        if (cfgChanged) {
            std::cout << "Config file changed..." << std::endl;
            loadConfig();
        }

        // Handle next event
        XNextEvent(display, &xevent);

        // Skip this completely if sticky edges aren't enabled
        switch (xevent.type) {
        case GenericEvent:
            if (cfgEnabled && XGetEventData(display, &xevent.xcookie)) {
                XGenericEventCookie *cookie = &xevent.xcookie;

                if (cookie->extension == xiExtOpcode && cookie->evtype == XI_RawMotion) {
                    // This is the event we were looking for
                    XIDeviceEvent *motionEvent = (XIDeviceEvent *)cookie->data;

                    Window childWnd, parentDummy;
                    int root_x, root_y, win_x, win_y;
                    unsigned int maskDummy;
                    XQueryPointer(display, rootWindow, &rootWindow, &childWnd, &root_x, &root_y,
                                  &win_x, &win_y, &maskDummy);

                    pointerSpeedChanged(motionEvent->time, root_x, root_y, motionEvent->event_x,
                                        motionEvent->event_y);
                    if (motionEvent->event_x != 0 || motionEvent->event_y != 0 || true) {
                        pointerPositionChanged(motionEvent->time, root_x, root_y);
                    }
                }
                XFreeEventData(display, cookie);
            }
            break;
        case MotionNotify:
            pointerPositionChanged(xevent.xbutton.time, xevent.xmotion.x_root,
                                   xevent.xmotion.y_root);
            break;
        case ButtonPress:
        case ButtonRelease: {
            // free the pointer
            unconfinePointer();

            // replay the event to the window under sursor
            currentMonitor->snapPosition(&xevent.xbutton.x_root, &xevent.xbutton.y_root);
            Window cursorWindow =
                getWindowAt(rootWindow, xevent.xbutton.x_root, xevent.xbutton.y_root);
            printf("Window under cursor: %x\n", (int)cursorWindow);
            Window childDummy;
            XTranslateCoordinates(display, rootWindow, cursorWindow, xevent.xbutton.x_root,
                                  xevent.xbutton.y_root, &xevent.xbutton.x, &xevent.xbutton.y,
                                  &childDummy);
            xevent.xbutton.window = cursorWindow;
            XSendEvent(display, cursorWindow, True, ButtonPressMask | ButtonReleaseMask, &xevent);
            XFlush(display);

            // notify of the change
            pointerPositionChanged(xevent.xbutton.time, xevent.xbutton.x_root,
                                   xevent.xbutton.y_root);
            break;
        }
        case ConfigureNotify:
            updateMonitorList();
            break;
        }
    }

    // --Clean up---
    if (inotifyCfgW != -1)
        inotify_rm_watch(inotifyFD, inotifyCfgW);

    return 0;
}
