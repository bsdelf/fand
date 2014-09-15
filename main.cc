#include <iostream>
#include <fstream>
#include <string>
#include <array>
#include <ctime>
#include <locale>
#include <limits>

#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <libutil.h>

/*
 * 1. CPU
 * 2. Mini PCI Module
 * 3. HDD
 * 4. GPU
 * 5. Built-in battery
 * 6. UltraBay battery
 * 7. Built-in battery
 * 8. UltraBay battery
 */

static const char* LOG_FILE = "/var/log/fand.log";
static const char* PID_FILE = "/var/run/fand.pid";

static const char* CTL_THERMAL = "dev.acpi_ibm.0.thermal";
static const char* CTL_FAN = "dev.acpi_ibm.0.fan";
static const char* CTL_FANLEVEL = "dev.acpi_ibm.0.fan_level";

static std::fstream LOG;
static bool QUIT = false;

#define TLOG LOG << DateTime() << " "

/* utils */
static std::string DateTime(const char* fmt = "%Y.%m.%d %T")
{
    // buffer size is limited to 100
    char buf[100];
    std::time_t t = std::time(nullptr);
    size_t len = std::strftime(buf, sizeof(buf), fmt, std::localtime(&t));
    return std::string(buf, len);
}

static bool FetchThermal(std::array<int, 8>& thermal)
{
    // verify, fetch
    size_t sz = 0;
    if (sysctlbyname(CTL_THERMAL, nullptr, &sz, nullptr, 0) != 0)
        return false;
    if (sz != sizeof(thermal))
        return false;
    if (sysctlbyname(CTL_THERMAL, thermal.data(), &sz, nullptr, 0) != 0)
        return false;
    return true;
}

/* signal handler */
static void OnSignal(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        TLOG << "about to quit" << std::endl;
        QUIT = true;
    }
}

/* fan operations */
static int SwitchToManual()
{
    // switch to manual mode
    int err = 0;
    int val = 1;

    err = sysctlbyname(CTL_FANLEVEL, nullptr, 0, &val, sizeof(val));
    if (err != 0) goto LABEL_FAILED;

    val = 0;
    err = sysctlbyname(CTL_FAN, nullptr, 0, &val, sizeof(val));
    if (err != 0) goto LABEL_FAILED;

    return 0;

LABEL_FAILED:
    TLOG << "can't switch to manual mode!" << std::endl;
    return -1;
}

static int SwitchToAuto()
{
    // recover to automatic mode
    int val = 1;
    int retval = sysctlbyname(CTL_FAN, nullptr, 0, &val, sizeof(val));
    if (retval != 0) {
        TLOG << "can't switch to automatic mode" << std::endl;
    }
    return retval;
}

static bool SwitchToLevel(int idx)
{
    // fetch
    int previdx = 0;
    size_t sz = sizeof(previdx);
    if (sysctlbyname(CTL_FANLEVEL, &previdx, &sz, nullptr, 0) != 0)
        return false;
    if (previdx == idx)
        return true;

    // update
    TLOG << previdx << " => " << idx;
    if (sysctlbyname(CTL_FANLEVEL, nullptr, 0, &idx, sizeof(idx)) == 0) {
        LOG << std::endl;
        return true;
    } else {
        LOG <<  " failed!" << std::endl;
        return true;
    }
}

/* level profile */
struct Profile {
    int level;
    int min;
    int max;
    int delay;
    int stick;

    Profile(int level, int min, int max, int delay, int stick):
        level(level),
        min(min),
        max(max),
        delay(delay),
        stick(stick),
        __delay(delay)
    { }

    bool Hit(int val) const {
        return (val > min && val <= max);
    }

    bool Hold(int val) {
        if (val > max + stick) {
            return false;
        }

        if (val <= min && --__delay < 0) {
            return false;
        }

        if (__delay != delay) {
            __delay = delay;
        }

        return true;
    }

private:
    int __delay;
};

/* main loop */
static int AdjustLoop()
{
    const int tick = 300;           // ms
    const int delay = 30*1000/tick; // ms => ticks
    const int stick = 5;            // degree

    // profiles
    Profile profiles[] {
        {   0, std::numeric_limits<int>::min()+stick, 30, delay, stick   },
        {   1, 30, 40, delay, stick   },
        {   2, 40, 50, delay, stick   },
        {   3, 50, std::numeric_limits<int>::max()-stick, delay, stick   }
    };
    Profile dummy { -1, -1, -1, 0, 0 };

    auto PickProfile = [&profiles, &dummy](int val) -> Profile& {
        for (auto& p: profiles) {
            if (p.Hit(val)) { return p; }
        }
        return std::ref(dummy); // should not be reached
    };

    // loop
    TLOG << "begin adjust" << std::endl;
    for (Profile& profile = dummy; ; ) {
        if (QUIT) {
            break;
        }

        std::array<int, 8> thermal;
        if (!FetchThermal(thermal)) {
            TLOG << "failed to read thermal info!" << std::endl;
            break;
        }

        if (!profile.Hold(thermal[0])) {
            profile = PickProfile(thermal[0]);
            SwitchToLevel(profile.level);
        }

        usleep(tick*1000);
    }
    TLOG << "end adjust" << std::endl << std::endl;

    return 0;
}

int main(int argc, char** argv)
{
    int retval = 0;
    int nice = -5;
    struct pidfh* pfh = nullptr;
    pid_t otherpid;

    if (getuid() != 0) {
        retval = -1;
        std::cerr << "not super user" << std::endl;
        goto LABEL_FAILED;
    }

    pfh = pidfile_open(PID_FILE, 0600, &otherpid);
    if (pfh == nullptr) {
        if (errno == EEXIST) {
            std::cerr << getprogname() << " already running, pid " << otherpid << std::endl;
            goto LABEL_FAILED;
        }
        std::cerr << "cannot open or create pidfile!" << std::endl;
    }

    // renice
    if (setpriority(PRIO_PROCESS, 0, nice) != 0) {
        std::cerr << "failed to renice myself!" << std::endl;
        goto LABEL_FAILED;
    }

    // daemonalize
    if (daemon(0, 0) != 0) {
        std::cerr << "cannot enter daemon mode, exiting!" << std::endl;
        goto LABEL_FAILED;
    }

    pidfile_write(pfh);

    // signals
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    // do it
    LOG.open(LOG_FILE, std::ios::out | std::ios::app);
    if (!LOG.is_open()) {
        std::cerr << "failed to open LOG file: " << LOG_FILE << std::endl;
        goto LABEL_FAILED;
    }
    if ((retval = SwitchToManual()) == 0) {
        retval = AdjustLoop();
    }
    retval = (SwitchToAuto() & retval);
    LOG.close();
    
LABEL_FAILED:
    pidfile_remove(pfh);
    return retval;
}
