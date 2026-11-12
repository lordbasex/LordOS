/* lid.h — the laptop's lid, from logind (issue #97, docs/DISPLAYS.md, "The lid").
 *
 * Reads org.freedesktop.login1.Manager's LidClosed on the Settings system bus (hw/sysbus.c) and follows
 * its PropertiesChanged (the property emits changes), believed only from the connection that owns
 * org.freedesktop.login1 (the bus sets the sender). Every change goes to display_set_lid(), which leaves
 * the built-in panel out of the plan while the lid is closed and another display is connected. Nothing
 * is grabbed or polled, and logind keeps its own lid handling (suspend, HandleLidSwitch*). Only
 * Properties.Get on the manager: nothing that needs privileges. */
#ifndef LORD_HW_LID_H
#define LORD_HW_LID_H

void lid_start(void);                   /* after sysbus_start */

#endif
