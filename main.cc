#include <iostream>
#include <algorithm>
#include <vector>
using namespace std;

#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/resource.h>

const char* const ctl_thermal = "dev.acpi_ibm.0.thermal";
const char* const ctl_fanlevel = "dev.acpi_ibm.0.fan_level";

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

int main()
{
    int level = 1;
    int thermal[8];
    size_t levelLen = sizeof(level);
    size_t thermalLen = sizeof(thermal);
    const int thermalCount = sizeof(thermal)/sizeof(thermal[0]);
    const int hold = 10*1000;   // ms
    const int tick = 500;       // ms
    const int holdtimes = hold/tick;
    const int nice = -10;

    if (setpriority(PRIO_PROCESS, 0, nice) != 0) {
        cout << "renice myself failed!" << endl;
        return 1;
    }

    //cout << sysctlbyname(ctl_fanlevel, nullptr, 0, &level, sizeof(level)) << endl;
    //return 0;

    {
        size_t len = 0;
        int err = sysctlbyname(ctl_thermal, nullptr, &len, nullptr, 0);
        if (err != 0) {
            cout << "sysctl failed!" << endl;
            return 1;
        } else if (len != sizeof(thermal)) {
            cout << "size mismatch!" << endl;
            return 1;
        }
    }

    for (int lv1times = 0, lv2times = 0; ; ) {
        usleep(tick*1000);

        int err = sysctlbyname(ctl_thermal, thermal, &thermalLen, nullptr, 0);

        // 255 => -1
        replace_if(thermal, thermal+thermalCount,
                   [](int thermal)->bool { return thermal >= 255; },
                   -1);

        // fetch current level
        err = sysctlbyname(ctl_fanlevel, &level, &levelLen, nullptr, 0);
        if (err != 0) {
            cout << "fetch level failed!" << endl;
            return 1;
        }

        // adjust it according to CPU
        auto& max = thermal[0];//*max_element(thermal, thermal+thermalCount);
        if (max < 40) {
            if (level != 1) {
                if (++lv1times < holdtimes)
                    continue;
                else
                    lv1times = 0;

                cout << level << " => 1" << endl;
                level = 1;
                sysctlbyname(ctl_fanlevel, nullptr, 0, &level, sizeof(level));
            }
        } else if (max > 48) {
            lv1times = 0;
            lv2times = 0;

            if (level != 3) {
                cout << level << " => 3" << endl;
                level = 3;
                sysctlbyname(ctl_fanlevel, nullptr, 0, &level, sizeof(level));
            }
        } else {
            lv1times = 0;
            if (level > 2) {
                if (++lv2times < holdtimes)
                    continue;
                else
                    lv2times = 0;
            }

            if (level != 2) {
                cout << level << " => 2" << endl;
                level = 2;
                sysctlbyname(ctl_fanlevel, nullptr, 0, &level, sizeof(level));
            }
        }
    }

    return 0;
}
