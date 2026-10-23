# HelixPrint Plugin

The HelixPrint plugin is a small add-on for Moonraker, the service on your printer that
HelixScreen, Mainsail and Fluidd all talk to. HelixScreen works without it. With it, the
printer itself takes on some jobs that a screen can only do while it is running and connected,
and some that a small screen cannot do well at all.

The plugin matters most when your HelixScreen is not attached to the printer: a BigTreeTech
K-Touch, a phone or tablet, or a desktop computer connected over the network. A screen like
that comes and goes, and the plugin keeps working while it is away. On a screen attached to the
printer (a Raspberry Pi touchscreen, or the printer's own display) the HelixScreen app runs
whenever the printer is on, even while the display sleeps, so the difference is smaller.

---

## Installing the Plugin

On the screen, open **Advanced** and tap **Install HelixPrint Plugin**. The row only appears
when HelixScreen has checked your printer and found no plugin.

- **On a screen attached to the printer**, HelixScreen installs the plugin directly. It
  installs the plugin version that matches your HelixScreen release.
- **On a screen that is not attached** (K-Touch, tablet, desktop), the screen cannot reach the
  printer's files, so it shows a one-line command instead. Run that command on the printer over
  SSH. It looks like this, with your HelixScreen version in both places:

  ```bash
  curl -fsSL https://raw.githubusercontent.com/prestonbrown/helixscreen/v1.2.0/moonraker-plugin/remote-install.sh | sh -s -- v1.2.0
  ```

  Copy the command the screen shows rather than this example, so the plugin matches your app.
  After HelixScreen updates, run the command it shows again to bring the plugin up to date.

The installer restarts Moonraker once, which disconnects every screen and web interface for a
few seconds. Once the plugin is installed, the Advanced screen shows **HelixPrint Plugin** and
**Uninstall HelixPrint Plugin** in place of the Install row.

K-Touch owners: see also [Install the HelixPrint plugin on your printer](install-esp32.md#install-the-helixprint-plugin-on-your-printer).

---

## What It Adds

### File analysis

Your printer reads every G-code file once, in the background, right after you upload it.
Nothing about the file or the print changes; HelixScreen just gets answers it would otherwise
have to work out itself.

- **A thumbnail for every file.** Files your slicer saved without a picture get one: a top-down
  view of the print, in its filament colors. It shows in HelixScreen's file list, and in
  Mainsail and Fluidd too.
- **Faster previews of large files.** The layer preview of a big file appears as soon as the
  file has downloaded, instead of after HelixScreen has read through all of it.
- **Filament per tool, sooner.** The amount and color of filament each tool uses are ready for
  the checks before a print starts.

**Setting:** none. It runs whenever the plugin is installed, and it only reads files.

**Without the plugin**, HelixScreen reads the files itself: thumbnails only when your slicer
saved one, and previews of large files take longer to appear.

**Attached or remote:** an attached screen gets all of the above. On the K-Touch the plugin is
the only source: it cannot read whole files itself, so thumbnails for files without one and the
filament amount per tool come only from the plugin.

A file you uploaded a moment ago may not have been read yet; HelixScreen then works it out
itself. Reading never slows a print: on a printer with a single-core processor it waits until no
print is running.

### Start options for prints from other apps

HelixScreen sends some of your print options at the start of every print it starts itself, for
example which toolheads unload their filament when the print ends (Snapmaker U1). A print
started from Mainsail, Fluidd, your slicer, or the printer's job queue would normally miss them.

**Setting:** **Settings > Printing > Apply start options to prints from other apps**. It is off
until you turn it on.

- **With the plugin**, the printer applies the options itself at every print start, whether or
  not any HelixScreen is connected.
- **Without the plugin**, or with a plugin too old for this feature or unable to run it on your
  printer, the switch still works, and HelixScreen applies the options itself whenever its app
  is running and connected to the printer. A screen attached to the printer covers almost every
  print: only one that starts while the printer is booting or HelixScreen is restarting is
  missed, so it shows no note. A remote screen covers prints only while it is connected, so there
  the switch says **Only while this app is connected** and offers **Install**, or asks you to
  update the plugin when an older one is installed.

With several HelixScreens on one printer, only one applies the options: the one attached to the
printer, or a remote one when no attached screen is connected. Prints HelixScreen starts itself,
and a print resumed after a power cut, are not affected: they already have their options.

### Faster starts for edited prints

Some print starts change the file before it prints: turning off a pre-print step the file runs
itself (such as bed mesh), adding a skip option to your start macro, or, on a few tool changers,
mapping filament to different tools. HelixScreen never edits your original file; it prints a
changed copy that keeps every other line of the file.

**With the plugin**, the copy is made on the printer itself. The screen sends only the list of
changes, so a large file starts without first being copied to the screen and back, and the
screen needs no free space for it.

**Without the plugin**, or with an older one, the screen downloads the file, changes it and
uploads the copy. The result is the same; it just takes longer for big files.

**Setting:** none. This works the same on an attached screen and a remote one; a remote screen
on a slow network gains the most.

If the file changed on the printer since HelixScreen read it (for example, you re-uploaded it
under the same name), the print is refused with an error instead of changing the wrong lines.
Select the file again and start it.

### Printer Events

Printer Events is a log of what happened to your printer, as opposed to what HelixScreen told
you: Klipper shutting down and the reason it gave, a lost connection to a controller board
(MCU), Klipper disconnecting and coming back, firmware restarts, a print pausing and what paused
it, filament runouts during a print, error messages from the printer, and how each print ended.

**Where:** tap the **Notifications** widget on the Home screen, then **Printer Events** at the
top. There is nothing to turn on. See [Printer Events](advanced.md#printer-events) for the view
itself.

- **With the plugin**, the printer keeps the record. It includes what happened while no
  HelixScreen was connected, and every screen connected to the printer shows the same list. The
  view says **Recorded by the HelixPrint plugin**.
- **Without the plugin**, each screen keeps its own record of what it saw while connected, so
  two screens can show different lists. The view says **Recorded by this screen while
  connected**. When a screen reconnects, it fills the gap from what the printer still remembers
  (recent errors, how recent prints ended, and recent Klipper shutdowns), marked
  **(recovered)**. Firmware restarts, a pause sent from another device, and filament runouts are
  only recorded with the plugin.

A screen attached to the printer is connected almost all the time, so its own record misses
little. A remote screen misses whatever happened while it was away, apart from what it recovers.

The K-Touch keeps no record of its own. Without the plugin its Printer Events view says
**Printer events need the HelixPrint plugin on this screen**.

### Saving settings to your printer's config

**Save** on the Input Shaper screen, the PID-to-MPC switch on the PID screen, and the probe
settings rows on the Probe screen write straight to your Klipper configuration and restart the
printer's firmware so the change takes effect.

**With the plugin**, the printer makes the change itself. It finds the exact file and line where
each setting is defined, even when your config is split across several included files, and
writes the file in one step, so a power cut mid-save cannot leave it half-written. If someone
changed the same setting from another screen or from Mainsail in the meantime, the save is
refused instead of overwriting their change. Before every save it copies each file it changes
into the hidden `.helix_backups` folder in your config folder, keeping the five most recent
copies of each file with the date and time in their names.

**Without the plugin**, HelixScreen downloads the file, changes it, keeps one copy in
`.helix_backups` while the firmware restarts, and uploads the result. That copy is deleted once
Klipper comes back.

**Either way**, if Klipper does not come back after the restart, HelixScreen puts the previous
file back, restarts again and tells you the change was undone. To restore an older copy by hand,
turn on hidden files in Mainsail's or Fluidd's file manager and copy the file out of
`.helix_backups`. Troubleshooting covers this under [When the Printer Errors or Disconnects](../TROUBLESHOOTING.md#when-the-printer-errors-or-disconnects-recovery-dialog).

**Setting:** none. The plugin is used whenever it is installed, on attached and remote screens
alike.

### Power-loss recovery

On printers HelixScreen supports for it (today, the QIDI Q2), the plugin saves the print's
position about once a second, whichever app started the print, so a print can resume after a
power cut. After the power comes back, HelixScreen offers the resume and walks you through it
one confirmed step at a time: checking the part is still on the bed,
heating the bed and nozzle together, checking the nozzle height, then continuing.

Saving is on by default when the printer keeps its data on eMMC or similar storage, and off on
a printer that runs from an SD card, since it adds wear to the card. The switch is
**Settings > Printing > Power-loss recovery**; without the plugin it is greyed out, and on
other printers it does not appear. See
[Settings > Printing](settings/printing.md#power-loss-recovery-and-start-options-for-other-apps)
for the switch and [Recovering After a Power Loss](printing.md#resuming-with-the-helixprint-plugin)
for the step-by-step resume.

---

## The Plugin Overview

**Advanced > HelixPrint Plugin** shows the plugin's version and every feature it runs on your
printer, each marked:

- **Active**: running.
- **Off**: available, but switched off in its setting.
- **Unavailable**: cannot run on this printer, with the reason the plugin gave.

Tap a feature that has a setting to go to the page where its switch is. The overview itself has
no switches. Features with no setting show their status only.

## What the Notes on a Setting Mean

A setting that depends on the plugin can show one of these under its switch:

| Note | What it means |
|------|---------------|
| **Requires the HelixPrint plugin** | The feature only works with the plugin, and your printer does not have it. The switch is greyed out; tap **Install** to add the plugin |
| **Requires a newer HelixPrint plugin** | The plugin is installed but too old for this feature. Update the plugin: on an attached screen HelixScreen keeps it matched to the app; on a remote screen, run the install command again |
| **Only while this app is connected. Install the HelixPrint plugin to cover every print.** | On a remote screen without the plugin. The switch works, but HelixScreen can only act on prints while this app is running and connected. Tap **Install** to cover every print |
| **Only while this app is connected. Update the HelixPrint plugin to cover every print.** | On a remote screen whose plugin is too old for this feature, or cannot run it on your printer. The switch works the same way; run the install command again to update the plugin |

A screen attached to the printer shows none of the last two notes: it applies those options
itself, so the switch simply works. Only a feature that works solely through the plugin, such as
power-loss recovery, is hidden when your printer cannot use it, and the overview says why.
