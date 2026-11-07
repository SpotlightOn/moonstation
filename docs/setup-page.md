# Setup page

Hold the finger still for 5 s. The box beeps, shows its address in the battery
row and serves `https://<address>/` with a self-signed certificate (the browser
warning on the first visit is expected). The page closes itself after 15 minutes
without a request, or immediately on another 5 s hold.

Everything the owner tunes lives there, applied without a reboot; only WiFi
waits for the next server start (the box restarts itself when it was configured
over its own access point). A changed login takes effect immediately and the
browser asks for the new one right after saving. The password and the weather
key are write-only: the page reports only whether one is stored, an empty field
keeps it, and a checkbox erases the WiFi password or the key.

The page can also hand the next boot to xiaozhi-esp32, which lives in its own
flash slot; the button stays disabled while that slot is empty. Without a
browser the same switch is a two second hold on the BOOT button, in both
directions ([dual-boot.md](dual-boot.md)).

The first setup over the box's own access point is described in the README
under *First setup*; the server internals are in
[development.md](development.md).
