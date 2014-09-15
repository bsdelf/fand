#include <iostream>
#include <fstream>
#include <string>
#include <array>
#include <algorithm>
#include <ctime>
#include <locale>
using namespace std;

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

static fstream LOG;
static bool QUIT = false;

#define TLOG LOG << DateTime() << " "

/* utils */
static string DateTime(const char* fmt = "%Y.%m.%d %T")
{
    // buffer size is limited to 100
    char buf[100];
    std::time_t t = std::time(nullptr);
    size_t len = std::strftime(buf, sizeof(buf), fmt, std::localtime(&t));
    return string(buf, len);
}

static bool FetchThermal(array<int, 8>& thermal)
{
    // verify, fetch, normalize
    size_t thermalLen = 0;
    if (sysctlbyname(CTL_THERMAL, nullptr, &thermalLen, nullptr, 0) != 0)
        return false;
    if (thermalLen != sizeof(thermal))
        return false;
    if (sysctlbyname(CTL_THERMAL, thermal.data(), &thermalLen, nullptr, 0) != 0)
        return false;
    replace_if(thermal.begin(), thermal.end(),
               [](int t)->bool { return t >= 255; },
               -1);
    return true;
}

/* level handler */
enum class EmHandleRet
{
    Ok,
    Err,
    Out
};

class LevelHandler
{
public:
    static int STICK_TIMES;
    static int STICK_TEMP;

    static bool TrySwitch(int level)
    {
        // fetch
        int prevLevel = 0;
        size_t levelLen = sizeof(prevLevel);
        if (sysctlbyname(CTL_FANLEVEL, &prevLevel, &levelLen, nullptr, 0) != 0)
            return false;
        if (prevLevel == level)
            return true;

        // update
        TLOG << prevLevel << " => " << level;
        if (sysctlbyname(CTL_FANLEVEL, nullptr, 0, &level, sizeof(level)) == 0) {
            LOG << endl;
            return true;
        } else {
            LOG <<  " failed!" << endl;
            return true;
        }
    }

public:
    int level;
    int min;
    int max;

    LevelHandler(int _level, int _min, int _max):
        level(_level), min(_min), max(_max)
    {
    }

    EmHandleRet Handle(int t)
    {
        if (t > max+STICK_TEMP) {
            escapeTimes = 0;
            return EmHandleRet::Out;
        }

        if (t <= min) {
            if (++escapeTimes >= STICK_TIMES) {
                escapeTimes = 0;
                return EmHandleRet::Out;
            }
        }

        return TrySwitch(level) ? EmHandleRet::Ok : EmHandleRet::Err;
    }

    bool InRange(int t) const
    {
        return (t > min && t <= max);
    }

private:
    int escapeTimes = 0;
};

int LevelHandler::STICK_TIMES = 0;
int LevelHandler::STICK_TEMP = 0;

/* signal handler */
static void OnSignal(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        TLOG << "about to quit" << endl;
        QUIT = true;
    }
}

/* fan operations */
static int FanManual()
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
    TLOG << "can't switch to manual mode!" << endl;
    return -1;
}

static int FanAuto()
{
    // recover to automatic mode
    int val = 1;
    int retval = sysctlbyname(CTL_FAN, nullptr, 0, &val, sizeof(val));
    if (retval != 0) {
        TLOG << "can't switch to automatic mode" << endl;
    }
    return retval;
}

static int FanAdjust()
{
    array<int, 8> thermal;
    const int hold = 30*1000;   // ms
    const int tick = 500;       // ms

    LevelHandler::STICK_TEMP = 5;
    LevelHandler::STICK_TIMES = hold/tick;

    // handlers
    LevelHandler handlers[] = {
        {   0,  -256,   30  },
        {   1,  30,     40  },
        {   2,  40,     50  },
        {   3,  50,     255 }
    };

    auto FnPickHandler = [&handlers](int t)->LevelHandler* {
        for (LevelHandler& h: handlers) {
            if (h.InRange(t))
                return &h;
        }
        return nullptr;
    };

    // loop
    TLOG << "begin adjust" << endl;
    FetchThermal(thermal);
    LevelHandler* inith = FnPickHandler(thermal[0]);
    TLOG << "initial level: " << inith->level << endl;
    for (LevelHandler* h = inith; h != nullptr; ) {
        if (QUIT)
            break;
        if (!FetchThermal(thermal)) {
            TLOG << "failed to read thermal info!" << endl;
            break;
        }

        int cpu = thermal[0];
        EmHandleRet retval = h->Handle(cpu); 
        switch (retval) {
        case EmHandleRet::Out: {
            LevelHandler* newh = FnPickHandler(cpu);
            if (newh != h) { // avoid busy Handle()
                h = newh;
                continue;
            }
        }
            break;

        case EmHandleRet::Ok:
            break;

        case EmHandleRet::Err:
        default: {
            return -1;
        }
            break;
        }

        usleep(tick*1000);
    }
    TLOG << "end adjust" << endl << endl;

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
        cerr << "not super user" << endl;
        goto LABEL_FAILED;
    }

    pfh = pidfile_open(PID_FILE, 0600, &otherpid);
    if (pfh == nullptr) {
        if (errno == EEXIST) {
            cerr << getprogname() << " already running, pid " << otherpid << endl;
            goto LABEL_FAILED;
        }
        cerr << "cannot open or create pidfile!" << endl;
    }

    // renice
    if (setpriority(PRIO_PROCESS, 0, nice) != 0) {
        cerr << "failed to renice myself!" << endl;
        goto LABEL_FAILED;
    }

    // daemonalize
    if (daemon(0, 0) != 0) {
        cerr << "cannot enter daemon mode, exiting!" << endl;
        goto LABEL_FAILED;
    }

    pidfile_write(pfh);

    // signals
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    // do it
    LOG.open(LOG_FILE, ios::out | ios::app);
    if (!LOG.is_open()) {
        cerr << "failed to open LOG file: " << LOG_FILE << endl;
        goto LABEL_FAILED;
    }
    if ((retval = FanManual()) == 0)
        retval = FanAdjust();
    retval = (FanAuto() & retval);
    LOG.close();
    
LABEL_FAILED:
    pidfile_remove(pfh);
    return retval;
}
