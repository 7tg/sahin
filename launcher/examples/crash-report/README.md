# Native crash report example

Adapted from BlackBearReloaded's ProsperoEden crash/restart flow. Call `install`
after elevation, with an existing writable directory, a version string, and a
lifecycle callback. The callback runs on a prestarted worker: restart on `true`,
close on `false`. Use `stop` during orderly teardown. `recovered` reports a
previous crash so the app can offer its saved report to the user.

The signal handler writes a bounded report without allocation, stdio, or locks.
It records signal, fault address, and x86-64 instruction/stack/frame pointers.
Only one automatic restart is attempted; a crash after that restart requests
closure. If the callback cannot leave, the original fatal signal is restored.
The application owns log rotation and presentation. A corrupted process may
still prevent reporting; this is diagnostic best effort, not transaction recovery.

`make test-crash-report` checks the report and restart-loop guard in isolated
host processes. The FreeBSD context offsets follow Eden's qualified layout;
each adopter must validate its native build on its target firmware.
